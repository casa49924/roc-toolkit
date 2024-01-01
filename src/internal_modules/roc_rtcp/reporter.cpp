/*
 * Copyright (c) 2023 Roc Streaming authors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "roc_rtcp/reporter.h"
#include "roc_core/log.h"
#include "roc_core/panic.h"
#include "roc_core/time.h"
#include "roc_packet/ntp.h"
#include "roc_packet/units.h"
#include "roc_rtcp/cname.h"
#include "roc_status/status_code.h"

namespace roc {
namespace rtcp {

Reporter::Reporter(const Config& config,
                   IStreamController& stream_controller,
                   core::IArena& arena)
    : stream_controller_(stream_controller)
    , stream_pool_("stream_pool", arena)
    , stream_map_(arena)
    , local_send_streams_(arena)
    , local_recv_streams_(arena)
    , local_recv_reports_(arena)
    , collision_detected_(false)
    , collision_reported_(false)
    , report_state_(State_Idle)
    , report_error_(status::StatusOK)
    , report_time_(0)
    , timeout_(config.inactivity_timeout)
    , valid_(false) {
    local_source_id_ = stream_controller_.source_id();
    memset(local_cname_, 0, sizeof(local_cname_));

    const char* cname = stream_controller_.cname();
    if (cname == NULL || cname[0] == '\0' || strlen(cname) > sizeof(local_cname_) - 1) {
        roc_log(LogError, "rtcp reporter: cname() should return short non-empty string");
        return;
    }
    strcpy(local_cname_, cname);

    roc_log(LogDebug,
            "rtcp reporter: initializing: local_ssrc=%lu local_cname=%s timeout=%.3fms",
            (unsigned long)local_source_id_, cname_to_str(local_cname_).c_str(),
            (double)timeout_ / core::Millisecond);

    valid_ = true;
}

Reporter::~Reporter() {
    roc_panic_if_msg(report_state_ != State_Idle,
                     "rtcp reporter: invalid state in destructor");
}

bool Reporter::is_valid() const {
    return valid_;
}

bool Reporter::is_sending() const {
    roc_panic_if(!is_valid());

    return stream_controller_.has_send_stream();
}

bool Reporter::is_receiving() const {
    roc_panic_if(!is_valid());

    return stream_controller_.num_recv_streams() != 0;
}

size_t Reporter::num_streams() const {
    roc_panic_if(!is_valid());

    return stream_map_.size();
}

status::StatusCode Reporter::begin_processing(core::nanoseconds_t report_time) {
    roc_panic_if(!is_valid());

    roc_panic_if_msg(report_state_ != State_Idle, "rtcp reporter: invalid call order");
    roc_panic_if_msg(report_time <= 0, "rtcp reporter: invalid timestamp");

    report_state_ = State_Processing;
    report_error_ = status::StatusOK;
    report_time_ = report_time;

    return status::StatusOK;
}

// Process SDES CNAME data generated by remote sender
void Reporter::process_cname(const SdesChunk& chunk, const SdesItem& item) {
    roc_panic_if_msg(report_state_ != State_Processing,
                     "rtcp reporter: invalid call order");

    // SSRC from SDES chunk is stream sender or receiver (RTCP packet originator)
    const packet::stream_source_t stream_source_id = chunk.ssrc;

    if (item.text == NULL || item.text[0] == '\0') {
        // Ignore empty CNAME.
        return;
    }

    // Report to local sending or receiving stream from remote receiver or sender
    core::SharedPtr<Stream> stream = find_stream_(stream_source_id, AutoCreate);
    if (!stream) {
        return;
    }

    if (strcmp(stream->cname, item.text) == 0) {
        // CNAME already up-to-date, nothing to do
        return;
    }

    roc_log(LogTrace, "rtcp reporter: processing CNAME: ssrc=%lu cname=%s",
            (unsigned long)stream_source_id, cname_to_str(item.text).c_str());

    if (stream->cname[0] != '\0') {
        // CNAME was not empty and changed, re-create stream.
        // Note that we parse SDES before SR/RR because if stream should be re-created, it
        // should be done before parsing reports and filling stream data.
        // This parsing order is maintained by rtcp::Communicator.
        roc_log(LogDebug,
                "rtcp reporter: detected CNAME change, recreating stream:"
                " ssrc=%lu old_cname=%s new_cname=%s",
                (unsigned long)stream->source_id, cname_to_str(stream->cname).c_str(),
                cname_to_str(item.text).c_str());

        roc_log(LogTrace, "rtcp reporter: halt_recv_stream(): ssrc=%lu cname=%s",
                (unsigned long)stream->source_id, cname_to_str(stream->cname).c_str());

        stream_controller_.halt_recv_stream(stream->source_id);

        remove_stream_(*stream);

        if (!(stream = find_stream_(stream_source_id, AutoCreate))) {
            return;
        }
    }

    // Update CNAME.
    strcpy(stream->cname, item.text);

    // Detect collisions after we've updated CNAME.
    detect_collision_(stream_source_id);

    update_stream_(*stream);
}

// Process SR data generated by remote sender
void Reporter::process_sr(const header::SenderReportPacket& sr) {
    roc_panic_if_msg(report_state_ != State_Processing,
                     "rtcp reporter: invalid call order");

    // SSRC from SR is stream sender (RTCP packet originator)
    const packet::stream_source_t send_source_id = sr.ssrc();

    detect_collision_(send_source_id);

    // Report to local receiving stream from remote sender
    core::SharedPtr<Stream> stream = find_stream_(send_source_id, AutoCreate);
    if (!stream) {
        return;
    }

    roc_log(LogTrace, "rtcp reporter: processing SR: ssrc=%lu",
            (unsigned long)send_source_id);

    stream->has_remote_send_report = true;

    stream->remote_send_report.sender_cname = stream->cname;
    stream->remote_send_report.sender_source_id = stream->source_id;

    stream->remote_send_report.report_timestamp = packet::ntp_2_unix(sr.ntp_timestamp());
    stream->remote_send_report.stream_timestamp = sr.rtp_timestamp();

    stream->remote_send_report.packet_count = sr.packet_count();
    stream->remote_send_report.byte_count = sr.byte_count();

    stream->last_sr = report_time_;

    update_stream_(*stream);
}

// Process SR/RR reception block data generated by remote receiver
// (in case of SR, remote peer acts as both sender & receiver)
void Reporter::process_reception_block(const packet::stream_source_t ssrc,
                                       const header::ReceptionReportBlock& blk) {
    roc_panic_if_msg(report_state_ != State_Processing,
                     "rtcp reporter: invalid call order");

    // SSRC from SR/RR is stream receiver (RTCP packet originator)
    // SSRC from reception block is stream sender (RTCP packet recipient)
    const packet::stream_source_t send_source_id = blk.ssrc();
    const packet::stream_source_t recv_source_id = ssrc;

    detect_collision_(recv_source_id);

    if (send_source_id != local_source_id_) {
        // This report is for different sender, not for us, so ignore it.
        // Typical for multicast sessions.
        return;
    }

    // Report to local sending stream from remote receiver
    core::SharedPtr<Stream> stream = find_stream_(recv_source_id, AutoCreate);
    if (!stream) {
        return;
    }

    roc_log(
        LogTrace,
        "rtcp reporter: processing SR/RR reception block: send_ssrc=%lu recv_ssrc=%lu",
        (unsigned long)send_source_id, (unsigned long)recv_source_id);

    stream->has_remote_recv_report = true;

    stream->remote_recv_report.receiver_cname = stream->cname;
    stream->remote_recv_report.receiver_source_id = stream->source_id;
    stream->remote_recv_report.sender_source_id = local_source_id_;

    stream->remote_recv_report.fract_loss = blk.fract_loss();
    stream->remote_recv_report.cum_loss = blk.cum_loss();
    stream->remote_recv_report.ext_last_seqnum = blk.last_seqnum();
    stream->remote_recv_report.jitter = blk.jitter();

    // TODO(gh-14): pass LSR and DLSR to RttEstimator

    update_stream_(*stream);
}

// Process XR DLRR data generated by remote sender
void Reporter::process_dlrr_subblock(const header::XrPacket& xr,
                                     const header::XrDlrrSubblock& blk) {
    roc_panic_if_msg(report_state_ != State_Processing,
                     "rtcp reporter: invalid call order");

    // SSRC from XR is stream sender (RTCP packet originator)
    // SSRC from DLRR sub-block is stream receiver (RTCP packet recipient)
    const packet::stream_source_t send_source_id = xr.ssrc();
    const packet::stream_source_t recv_source_id = blk.ssrc();

    detect_collision_(send_source_id);

    if (recv_source_id != local_source_id_) {
        // This report is for different receiver, not for us, so ignore it.
        // Typical for multicast sessions.
        return;
    }

    // Report to local receiving stream from remote sender.
    core::SharedPtr<Stream> stream = find_stream_(send_source_id, NoAutoCreate);
    if (!stream || !stream->has_remote_send_report) {
        // Ignore DLRR if there was no matching SR.
        // DLRR without SR is not very useful.
        return;
    }

    roc_log(LogTrace,
            "rtcp reporter: processing DLRR subblock: send_ssrc=%lu recv_ssrc=%lu",
            (unsigned long)send_source_id, (unsigned long)recv_source_id);

    // TODO(gh-14): pass LRR and DLRR to RttEstimator

    update_stream_(*stream);
}

// Process XR RRTR data generated by remote receiver
void Reporter::process_rrtr_block(const header::XrPacket& xr,
                                  const header::XrRrtrBlock& blk) {
    roc_panic_if_msg(report_state_ != State_Processing,
                     "rtcp reporter: invalid call order");

    // SSRC from XR is stream receiver (RTCP packet originator)
    const packet::stream_source_t recv_source_id = xr.ssrc();

    detect_collision_(recv_source_id);

    // Report to local sending stream from remote receiver.
    core::SharedPtr<Stream> stream = find_stream_(recv_source_id, NoAutoCreate);
    if (!stream || !stream->has_remote_recv_report) {
        // Ignore RRTR if there was no matching SR/RR reception block.
        // RRTR does not contain sender SSRC, so to decide whether this report is
        // for us, we update only streams already matched by reception blocks.
        return;
    }

    roc_log(LogTrace, "rtcp reporter: processing RRTR block: recv_ssrc=%lu",
            (unsigned long)recv_source_id);

    stream->remote_recv_report.report_timestamp = packet::ntp_2_unix(blk.ntp_timestamp());

    // Although it's called time of last RR in RFC, actually it's time of
    // last RRTR (which usually comes with RR).
    stream->last_rr = report_time_;

    update_stream_(*stream);
}

// Process BYE message generated by sender
void Reporter::process_goodbye(const packet::stream_source_t ssrc) {
    roc_panic_if_msg(report_state_ != State_Processing,
                     "rtcp reporter: invalid call order");

    // SSRC from BYE is stream sender (RTCP packet originator)
    const packet::stream_source_t send_source_id = ssrc;

    // Terminate local receiving stream after receiving BYE message from remote sender.
    core::SharedPtr<Stream> stream = find_stream_(send_source_id, NoAutoCreate);

    roc_log(LogDebug,
            "rtcp reporter: received BYE message, terminating stream:"
            " ssrc=%lu cname=%s",
            (unsigned long)ssrc, cname_to_str(stream ? stream->cname : NULL).c_str());

    if (stream) {
        remove_stream_(*stream);
    }

    roc_log(LogTrace, "rtcp reporter: halt_recv_stream(): send_ssrc=%lu send_cname=%s",
            (unsigned long)send_source_id,
            cname_to_str(stream ? stream->cname : NULL).c_str());

    stream_controller_.halt_recv_stream(send_source_id);
}

status::StatusCode Reporter::end_processing() {
    roc_panic_if_msg(report_state_ != State_Processing,
                     "rtcp reporter: invalid call order");

    // Detect and remove timed-out streams.
    detect_timeouts_();

    // Notify stream controller with updated reports.
    const status::StatusCode code = notify_streams_();

    report_state_ = State_Idle;

    if (code != status::StatusOK) {
        return code;
    }

    return report_error_;
}

status::StatusCode Reporter::begin_generation(const core::nanoseconds_t report_time) {
    roc_panic_if(!is_valid());

    roc_panic_if_msg(report_state_ != State_Idle, "rtcp reporter: invalid call order");
    roc_panic_if_msg(report_time <= 0, "rtcp reporter: invalid timestamp");

    report_state_ = State_Generating;
    report_error_ = status::StatusOK;
    report_time_ = report_time;

    // Query up-to-date reports from stream controller.
    status::StatusCode status;
    if ((status = query_send_streams_()) != status::StatusOK) {
        report_state_ = State_Idle;
        return status;
    }
    if ((status = query_recv_streams_()) != status::StatusOK) {
        report_state_ = State_Idle;
        return status;
    }

    return status::StatusOK;
}

// Generate SDES CNAME to deliver to remote receiver
void Reporter::generate_cname(SdesChunk& chunk, SdesItem& item) {
    roc_panic_if_msg(report_state_ != State_Generating,
                     "rtcp reporter: invalid call order");

    chunk.ssrc = local_source_id_;

    item.type = header::SDES_CNAME;
    item.text = local_cname_;
}

// Generate SR to deliver to remote receiver
void Reporter::generate_sr(header::SenderReportPacket& sr) {
    roc_panic_if_msg(report_state_ != State_Generating,
                     "rtcp reporter: invalid call order");

    roc_panic_if_msg(!is_sending(), "rtcp reporter: SR can be generated only by sender");

    sr.set_ssrc(local_source_id_);

    sr.set_ntp_timestamp(packet::unix_2_ntp(local_send_report_.report_timestamp));
    sr.set_rtp_timestamp(local_send_report_.stream_timestamp);

    sr.set_packet_count(local_send_report_.packet_count);
    sr.set_byte_count(local_send_report_.byte_count);
}

// Generate RR to deliver to remote sender
void Reporter::generate_rr(header::ReceiverReportPacket& rr) {
    roc_panic_if_msg(report_state_ != State_Generating,
                     "rtcp reporter: invalid call order");

    rr.set_ssrc(local_source_id_);
}

// Get number of reception blocks in SR or RR
size_t Reporter::num_reception_blocks() const {
    roc_panic_if(!is_valid());

    return local_recv_streams_.size();
}

// Generate SR/RR reception block to deliver to remote sender
void Reporter::generate_reception_block(size_t index, header::ReceptionReportBlock& blk) {
    roc_panic_if_msg(report_state_ != State_Generating,
                     "rtcp reporter: invalid call order");

    roc_panic_if_msg(
        !is_receiving(),
        "rtcp reporter: SR/RR reception block can be generated only by receiver");

    Stream* stream = local_recv_streams_[index];
    roc_panic_if(!stream);

    blk.set_ssrc(stream->source_id);

    blk.set_last_seqnum(stream->local_recv_report.ext_last_seqnum);
    blk.set_fract_loss(stream->local_recv_report.fract_loss);
    blk.set_cum_loss(stream->local_recv_report.cum_loss);
    blk.set_jitter(stream->local_recv_report.jitter);

    // LSR: ntp timestamp from last SR in remote clock domain
    blk.set_last_sr(packet::unix_2_ntp(stream->remote_send_report.report_timestamp));
    // DLSR: delay since last SR in local clock domain
    blk.set_delay_last_sr(packet::unix_2_ntp(report_time_ - stream->last_sr));
}

// Generate XR header to deliver to remote receiver or sender
void Reporter::generate_xr(header::XrPacket& xr) {
    roc_panic_if_msg(report_state_ != State_Generating,
                     "rtcp reporter: invalid call order");

    roc_panic_if_msg(!is_sending() && !is_receiving(),
                     "rtcp reporter: XR can be generated only by sender or receiver");

    xr.set_ssrc(local_source_id_);
}

// Get number of XR DLRR sub-blocks
size_t Reporter::num_dlrr_subblocks() const {
    roc_panic_if(!is_valid());

    return local_send_streams_.size();
}

// Generate XR DLRR sub-block to deliver to remote receiver
void Reporter::generate_dlrr_subblock(size_t index, header::XrDlrrSubblock& blk) {
    roc_panic_if_msg(report_state_ != State_Generating,
                     "rtcp reporter: invalid call order");

    roc_panic_if_msg(!is_sending(),
                     "rtcp reporter: DLRR can be generated only by sender");

    Stream* stream = local_send_streams_[index];
    roc_panic_if(!stream);

    blk.set_ssrc(stream->source_id);

    // LRR: ntp timestamp from last RR in remote clock domain
    blk.set_last_rr(packet::unix_2_ntp(stream->remote_recv_report.report_timestamp));
    // DLRR: delay since last RR in local clock domain
    blk.set_delay_last_rr(packet::unix_2_ntp(report_time_ - stream->last_rr));
}

// Generate XR RRTR header to deliver to remote sender
void Reporter::generate_rrtr_block(header::XrRrtrBlock& blk) {
    roc_panic_if_msg(report_state_ != State_Generating,
                     "rtcp reporter: invalid call order");

    roc_panic_if_msg(!is_receiving(),
                     "rtcp reporter: RRTR can be generated only by receiver");

    // RRTR blocks are identical for all receiving streams,
    // so we can just use the first one.
    Stream* stream = local_recv_streams_[0];
    roc_panic_if(!stream);

    blk.set_ntp_timestamp(packet::unix_2_ntp(stream->local_recv_report.report_timestamp));
}

bool Reporter::need_goodbye() const {
    return collision_detected_;
}

void Reporter::generate_goodbye(packet::stream_source_t& ssrc) {
    roc_panic_if_msg(report_state_ != State_Generating,
                     "rtcp reporter: invalid call order");

    ssrc = local_source_id_;

    if (collision_detected_) {
        collision_reported_ = true;
    }
}

status::StatusCode Reporter::end_generation() {
    roc_panic_if_msg(report_state_ != State_Generating,
                     "rtcp reporter: invalid call order");

    report_state_ = State_Idle;

    // Resolve SSRC collision.
    // We do it after generation when we already sent BYE packet.
    resolve_collision_();

    // Detect and remove timed-out streams.
    detect_timeouts_();

    return report_error_;
}

status::StatusCode Reporter::notify_streams_() {
    if (report_time_ == 0) {
        return status::StatusOK;
    }

    const bool is_sending = stream_controller_.has_send_stream();

    for (Stream* stream = lru_streams_.front(); stream != NULL;
         stream = lru_streams_.nextof(*stream)) {
        // Recently updated streams are moved to the front of the list.
        // We iterate from front to back and stop when we find first stream which
        // was updated before current report.
        if (stream->last_update < report_time_) {
            break;
        }

        if (stream->has_remote_recv_report && is_sending) {
            // If we're sending, notify pipeline about every discovered
            // receiver report for our sending stream.
            const packet::stream_source_t recv_source_id = stream->source_id;

            roc_log(LogTrace,
                    "rtcp reporter:"
                    " notify_send_stream(): send_ssrc=%lu recv_ssrc=%lu",
                    (unsigned long)local_source_id_, (unsigned long)recv_source_id);

            const status::StatusCode code = stream_controller_.notify_send_stream(
                recv_source_id, stream->remote_recv_report);

            if (code != status::StatusOK) {
                roc_log(LogError,
                        "rtcp reporter:"
                        " failed to notify send stream: send_ssrc=%lu recv_ssrc=%lu",
                        (unsigned long)local_source_id_, (unsigned long)recv_source_id);
                return code;
            }
        }

        if (stream->has_remote_send_report) {
            // If we're receiving, notify pipeline about every discovered
            // sender report for one of our receiving streams.
            const packet::stream_source_t send_source_id = stream->source_id;

            roc_log(LogTrace,
                    "rtcp reporter:"
                    " notify_recv_stream(): recv_ssrc=%lu send_ssrc=%lu send_cname=%s",
                    (unsigned long)local_source_id_, (unsigned long)send_source_id,
                    cname_to_str(stream->cname).c_str());

            const status::StatusCode code = stream_controller_.notify_recv_stream(
                send_source_id, stream->remote_send_report);

            if (code != status::StatusOK) {
                roc_log(LogError,
                        "rtcp reporter:"
                        " failed to notify recv stream: recv_ssrc=%lu send_ssrc=%lu",
                        (unsigned long)local_source_id_, (unsigned long)send_source_id);
                return code;
            }
        }
    }

    return status::StatusOK;
}

status::StatusCode Reporter::query_send_streams_() {
    local_send_streams_.clear();

    if (stream_controller_.has_send_stream()) {
        local_send_report_ = stream_controller_.query_send_stream(report_time_);

        roc_log(LogTrace,
                "rtcp reporter: query_send_stream(): send_ssrc=%lu send_cname=%s",
                (unsigned long)local_send_report_.sender_source_id,
                cname_to_str(local_send_report_.sender_cname).c_str());

        validate_send_report_(local_send_report_);

        // We have only one actual sending stream, but in case of multicast,
        // we create multiple sending stream objects, one per each receiver.
        // We know about present receivers from RTCP reports obtained from
        // them. Here we iterate over every sending stream object for each
        // discovered receiver and update its sending report. This report will
        // be later used to generate RTCP packets for that specific receiver.
        for (core::SharedPtr<Stream> stream = stream_map_.front(); stream != NULL;
             stream = stream_map_.nextof(*stream)) {
            if (stream->has_remote_recv_report) {
                stream->local_send_report = local_send_report_;

                if (!local_send_streams_.push_back(stream.get())) {
                    return status::StatusNoMem;
                }
            }
        }
    }

    return status::StatusOK;
}

status::StatusCode Reporter::query_recv_streams_() {
    const size_t recv_count = stream_controller_.num_recv_streams();

    if (!local_recv_reports_.resize(recv_count)) {
        return status::StatusNoMem;
    }

    if (recv_count != 0) {
        roc_log(LogTrace, "rtcp reporter: query_recv_streams(): n_reports=%lu",
                (unsigned long)recv_count);

        stream_controller_.query_recv_streams(local_recv_reports_.data(),
                                              local_recv_reports_.size(), report_time_);
    }

    local_recv_streams_.clear();

    // We can have multiple receiving streams, one per each sender in session.
    // Here we iterate over every receiving stream reported by pipeline and
    // update its receiving report. This report will be later used to generate
    // RTCP packets for corresponding sender.
    for (size_t recv_idx = 0; recv_idx < recv_count; recv_idx++) {
        validate_recv_report_(local_recv_reports_[recv_idx]);

        core::SharedPtr<Stream> stream =
            find_stream_(local_recv_reports_[recv_idx].sender_source_id, AutoCreate);
        if (!stream) {
            return status::StatusNoMem;
        }

        stream->local_recv_report = local_recv_reports_[recv_idx];

        if (!local_recv_streams_.push_back(stream.get())) {
            return status::StatusNoMem;
        }
    }

    return status::StatusOK;
}

void Reporter::detect_timeouts_() {
    if (report_time_ == 0) {
        return;
    }

    // If stream was not updated after deadline, it should be removed.
    const core::nanoseconds_t deadline = report_time_ - timeout_;

    while (Stream* stream = lru_streams_.back()) {
        // Recently updated streams are moved to the front of the list.
        // We iterate from back to front and stop when we find first stream which
        // was updated recently enough.
        if (stream->last_update > deadline) {
            break;
        }

        roc_log(LogDebug,
                "rtcp reporter: report timeout expired, terminating stream:"
                " ssrc=%lu cname=%s timeout=%.3fms",
                (unsigned long)stream->source_id, cname_to_str(stream->cname).c_str(),
                (double)timeout_ / core::Microsecond);

        {
            // If we're receiving, notify pipeline that sender timed out.
            const packet::stream_source_t send_source_id = stream->source_id;

            roc_log(LogTrace,
                    "rtcp reporter: halt_recv_stream(): send_ssrc=%lu send_cname=%s",
                    (unsigned long)send_source_id, cname_to_str(stream->cname).c_str());

            stream_controller_.halt_recv_stream(send_source_id);
        }

        remove_stream_(*stream);
    }
}

void Reporter::detect_collision_(packet::stream_source_t source_id) {
    if (collision_detected_) {
        // Already detected collision.
        return;
    }

    if (source_id != local_source_id_) {
        // No collision.
        return;
    }

    core::SharedPtr<Stream> stream = find_stream_(source_id, NoAutoCreate);

    if (stream && stream->is_looped) {
        // We already detected that this is not a collision (see below).
        return;
    }

    if (stream && strcmp(stream->cname, local_cname_) == 0) {
        // If stream has same both SSRC and CNAME, we assume that this is our
        // own stream looped back to us because of network misconfiguration.
        // We report it once and don't consider it a collision.
        roc_log(LogDebug,
                "rtcp reporter: detected possible network loop:"
                " remote stream has same SSRC and CNAME as local: ssrc=%lu cname=%s",
                (unsigned long)stream->source_id, cname_to_str(stream->cname).c_str());
        stream->is_looped = true;
        return;
    }

    // If stream has same SSRC, but not CNAME, we assume that we found SSRC
    // collision. We should change our own SSRC to a new random value.
    // It will be done in resolve_collision_() later.
    roc_log(LogDebug,
            "rtcp reporter: detected SSRC collision:"
            " remote_ssrc=%lu remote_cname=%s local_ssrc=%lu local_cname=%s",
            (unsigned long)source_id, cname_to_str(stream ? stream->cname : NULL).c_str(),
            (unsigned long)local_source_id_, cname_to_str(local_cname_).c_str());

    collision_detected_ = true;
    collision_reported_ = false;
}

void Reporter::resolve_collision_() {
    if (!collision_detected_) {
        return;
    }

    if (!collision_reported_) {
        roc_panic("rtcp reporter:"
                  " need_goodbye() returned true, but generate_goodbye() was not called");
    }

    for (;;) {
        stream_controller_.change_source_id();

        local_source_id_ = stream_controller_.source_id();

        core::SharedPtr<Stream> stream = find_stream_(local_source_id_, NoAutoCreate);
        if (stream) {
            // Newly selected SSRC leads to collision again.
            // Repeat.
            continue;
        }

        break;
    }

    roc_log(LogDebug, "rtcp reporter: changed local SSRC: ssrc=%lu cname=%s",
            (unsigned long)local_source_id_, cname_to_str(local_cname_).c_str());

    for (core::SharedPtr<Stream> stream = stream_map_.front(); stream != NULL;
         stream = stream_map_.nextof(*stream)) {
        if (stream->has_remote_recv_report) {
            stream->remote_recv_report.sender_source_id = local_source_id_;
        }
    }

    collision_detected_ = false;
    collision_reported_ = false;
}

void Reporter::validate_send_report_(const SendReport& send_report) {
    if (send_report.sender_source_id != local_source_id_) {
        roc_panic("rtcp reporter:"
                  " query returned invalid sender report:"
                  " sender SSRC should be same returned by source_id():"
                  " returned_ssrc=%lu expected_ssrc=%lu",
                  (unsigned long)send_report.sender_source_id,
                  (unsigned long)local_source_id_);
    }

    if (send_report.sender_cname == NULL) {
        roc_panic("rtcp reporter:"
                  " query returned invalid sender report:"
                  " CNAME is null");
    }

    if (strcmp(send_report.sender_cname, local_cname_) != 0) {
        roc_panic("rtcp reporter:"
                  " query returned invalid sender report:"
                  " CNAME should be same as returned by cname():"
                  " returned_cname=%s expected_cname=%s",
                  cname_to_str(send_report.sender_cname).c_str(),
                  cname_to_str(local_cname_).c_str());
    }

    if (send_report.report_timestamp <= 0) {
        roc_panic("rtcp reporter:"
                  " query returned invalid sender report:"
                  " report timestamp should be positive");
    }
}

void Reporter::validate_recv_report_(const RecvReport& recv_report) {
    if (recv_report.receiver_source_id != local_source_id_) {
        roc_panic("rtcp reporter:"
                  " query returned invalid receiver report:"
                  " SSRC should be same for all local sending and receiving streams:"
                  " returned_ssrc=%lu expected_ssrc=%lu",
                  (unsigned long)recv_report.receiver_source_id,
                  (unsigned long)local_source_id_);
    }

    if (recv_report.report_timestamp <= 0) {
        roc_panic("rtcp reporter:"
                  " query returned invalid receiver report:"
                  " report timestamp should be positive");
    }
}

core::SharedPtr<Reporter::Stream>
Reporter::find_stream_(packet::stream_source_t source_id, CreateMode mode) {
    core::SharedPtr<Stream> stream = stream_map_.find(source_id);

    if (!stream && mode == NoAutoCreate) {
        return NULL;
    }

    if (!stream) {
        roc_log(LogDebug, "rtcp reporter: creating stream: ssrc=%lu",
                (unsigned long)source_id);

        stream = new (stream_pool_) Stream(stream_pool_, source_id, report_time_);
        if (!stream) {
            report_error_ = status::StatusNoMem;
            return NULL;
        }

        if (!stream_map_.insert(*stream)) {
            report_error_ = status::StatusNoMem;
            return NULL;
        }
    }

    return stream;
}

void Reporter::remove_stream_(Stream& stream) {
    roc_log(LogDebug, "rtcp reporter: removing stream: ssrc=%lu",
            (unsigned long)stream.source_id);

    if (lru_streams_.contains(stream)) {
        lru_streams_.remove(stream);
    }

    stream_map_.remove(stream);
}

void Reporter::update_stream_(Stream& stream) {
    stream.last_update = report_time_;

    if (lru_streams_.contains(stream)) {
        lru_streams_.remove(stream);
    }

    lru_streams_.push_front(stream);
}

} // namespace rtcp
} // namespace roc
