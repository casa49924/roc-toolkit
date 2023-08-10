/*
 * Copyright (c) 2023 Roc Streaming authors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "roc_node/receiver_decoder.h"
#include "roc_address/interface.h"
#include "roc_core/log.h"
#include "roc_core/panic.h"

namespace roc {
namespace node {

ReceiverDecoder::ReceiverDecoder(Context& context,
                                 const pipeline::ReceiverConfig& pipeline_config)
    : Node(context)
    , pipeline_(*this,
                pipeline_config,
                context.format_map(),
                context.packet_factory(),
                context.byte_buffer_factory(),
                context.sample_buffer_factory(),
                context.arena())
    , slot_(NULL)
    , processing_task_(pipeline_)
    , valid_(false) {
    roc_log(LogDebug, "receiver decoder node: initializing");

    if (!pipeline_.is_valid()) {
        roc_log(LogError, "receiver decoder node: failed to construct pipeline");
        return;
    }

    pipeline::ReceiverLoop::Tasks::CreateSlot slot_task;
    if (!pipeline_.schedule_and_wait(slot_task)) {
        roc_log(LogError, "receiver decoder node: failed to create slot");
        return;
    }

    slot_ = slot_task.get_handle();
    if (!slot_) {
        roc_log(LogError, "receiver decoder node: failed to create slot");
        return;
    }

    valid_ = true;
}

ReceiverDecoder::~ReceiverDecoder() {
    roc_log(LogDebug, "receiver decoder node: deinitializing");

    if (slot_) {
        // First remove slot. This may involve usage of processing task.
        pipeline::ReceiverLoop::Tasks::DeleteSlot task(slot_);
        if (!pipeline_.schedule_and_wait(task)) {
            roc_panic("receiver decoder node: can't remove pipeline slot");
        }
    }

    // Then wait until processing task is fully completed, before
    // proceeding to its destruction.
    context().control_loop().wait(processing_task_);
}

bool ReceiverDecoder::is_valid() {
    return valid_;
}

bool ReceiverDecoder::bind(address::Interface iface, address::Protocol proto) {
    core::Mutex::Lock lock(mutex_);

    roc_panic_if_not(is_valid());

    roc_panic_if(iface < 0);
    roc_panic_if(iface >= (int)address::Iface_Max);

    roc_log(LogInfo, "receiver decoder node: binding %s interface to %s",
            address::interface_to_str(iface), address::proto_to_str(proto));

    pipeline::ReceiverLoop::Tasks::AddEndpoint endpoint_task(slot_, iface, proto);
    if (!pipeline_.schedule_and_wait(endpoint_task)) {
        roc_log(LogError,
                "receiver decoder node:"
                " can't connect %s interface: can't add endpoint to pipeline",
                address::interface_to_str(iface));
        return false;
    }

    endpoint_writers_[iface] = endpoint_task.get_writer();

    return true;
}

void ReceiverDecoder::write(address::Interface iface, const packet::PacketPtr& packet) {
    roc_panic_if_not(is_valid());

    roc_panic_if(iface < 0);
    roc_panic_if(iface >= (int)address::Iface_Max);

    if (packet::IWriter* writer = endpoint_writers_[iface]) {
        writer->write(packet);
    }
}

sndio::ISource& ReceiverDecoder::source() {
    roc_panic_if_not(is_valid());

    return pipeline_.source();
}

void ReceiverDecoder::schedule_task_processing(pipeline::PipelineLoop&,
                                               core::nanoseconds_t deadline) {
    context().control_loop().schedule_at(processing_task_, deadline, NULL);
}

void ReceiverDecoder::cancel_task_processing(pipeline::PipelineLoop&) {
    context().control_loop().async_cancel(processing_task_);
}

} // namespace node
} // namespace roc
