/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "reference_queue.h"

#include "accounting/card_table-inl.h"
#include "collector/concurrent_copying.h"
#include "heap.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "mirror/reference-inl.h"

namespace art {
namespace gc {

ReferenceQueue::ReferenceQueue(Mutex* lock) : lock_(lock), list_(nullptr) {
}

void ReferenceQueue::AtomicEnqueueIfNotEnqueued(Thread* self, mirror::Reference* ref) {
  DCHECK(ref != nullptr);
  MutexLock mu(self, *lock_);
  if (!ref->IsEnqueued()) {
    EnqueuePendingReference(ref);
  }
}

void ReferenceQueue::EnqueueReference(mirror::Reference* ref) {
  CHECK(ref->IsEnqueuable());
  EnqueuePendingReference(ref);
}

void ReferenceQueue::EnqueuePendingReference(mirror::Reference* ref) {
  DCHECK(ref != nullptr);
  if (IsEmpty()) {
    // 1 element cyclic queue, ie: Reference ref = ..; ref.pendingNext = ref;
    list_ = ref;
  } else {
    mirror::Reference* head = list_->GetPendingNext();
    if (Runtime::Current()->IsActiveTransaction()) {
      ref->SetPendingNext<true>(head);
    } else {
      ref->SetPendingNext<false>(head);
    }
  }
  if (Runtime::Current()->IsActiveTransaction()) {
    list_->SetPendingNext<true>(ref);
  } else {
    list_->SetPendingNext<false>(ref);
  }
}

mirror::Reference* ReferenceQueue::DequeuePendingReference() {
  DCHECK(!IsEmpty());
  mirror::Reference* head = list_->GetPendingNext();
  DCHECK(head != nullptr);
  mirror::Reference* ref;
  // Note: the following code is thread-safe because it is only called from ProcessReferences which
  // is single threaded.
  if (list_ == head) {
    ref = list_;
    list_ = nullptr;
  } else {
    mirror::Reference* next = head->GetPendingNext();
    if (Runtime::Current()->IsActiveTransaction()) {
      list_->SetPendingNext<true>(next);
    } else {
      list_->SetPendingNext<false>(next);
    }
    ref = head;
  }
  if (Runtime::Current()->IsActiveTransaction()) {
    ref->SetPendingNext<true>(nullptr);
  } else {
    ref->SetPendingNext<false>(nullptr);
  }
  Heap* heap = Runtime::Current()->GetHeap();
  if (kUseBakerOrBrooksReadBarrier && heap->CurrentCollectorType() == kCollectorTypeCC &&
      heap->ConcurrentCopyingCollector()->IsActive()) {
    // Clear the gray ptr we left in ConcurrentCopying::ProcessMarkStack().
    // We don't want to do this when the zygote compaction collector (SemiSpace) is running.
    CHECK(ref != nullptr);
    CHECK_EQ(ref->GetReadBarrierPointer(), ReadBarrier::GrayPtr())
        << "ref=" << ref << " rb_ptr=" << ref->GetReadBarrierPointer();
    if (heap->ConcurrentCopyingCollector()->RegionSpace()->IsInToSpace(ref)) {
      // Moving objects.
      ref->AtomicSetReadBarrierPointer(ReadBarrier::GrayPtr(), ReadBarrier::WhitePtr());
      CHECK_EQ(ref->GetReadBarrierPointer(), ReadBarrier::WhitePtr());
    } else {
      // Non-moving objects.
      ref->AtomicSetReadBarrierPointer(ReadBarrier::GrayPtr(), ReadBarrier::BlackPtr());
      CHECK_EQ(ref->GetReadBarrierPointer(), ReadBarrier::BlackPtr());
    }
  }
  return ref;
}

void ReferenceQueue::Dump(std::ostream& os) const {
  mirror::Reference* cur = list_;
  os << "Reference starting at list_=" << list_ << "\n";
  if (cur == nullptr) {
    return;
  }
  do {
    mirror::Reference* pending_next = cur->GetPendingNext();
    os << "Reference= " << cur << " PendingNext=" << pending_next;
    if (cur->IsFinalizerReferenceInstance()) {
      os << " Zombie=" << cur->AsFinalizerReference()->GetZombie();
    }
    os << "\n";
    cur = pending_next;
  } while (cur != list_);
}

size_t ReferenceQueue::GetLength() const {
  size_t count = 0;
  mirror::Reference* cur = list_;
  if (cur != nullptr) {
    do {
      ++count;
      cur = cur->GetPendingNext();
    } while (cur != list_);
  }
  return count;
}

void ReferenceQueue::ClearWhiteReferences(ReferenceQueue* cleared_references,
                                          collector::GarbageCollector* collector) {
  while (!IsEmpty()) {
    mirror::Reference* ref = DequeuePendingReference();
    mirror::HeapReference<mirror::Object>* referent_addr = ref->GetReferentReferenceAddr();
    if (referent_addr->AsMirrorPtr() != nullptr &&
        !collector->IsMarkedHeapReference(referent_addr)) {
      // Referent is white, clear it.
      if (Runtime::Current()->IsActiveTransaction()) {
        ref->ClearReferent<true>();
      } else {
        ref->ClearReferent<false>();
      }
      if (ref->IsEnqueuable()) {
        cleared_references->EnqueuePendingReference(ref);
      }
    }
  }
}

void ReferenceQueue::EnqueueFinalizerReferences(ReferenceQueue* cleared_references,
                                                collector::GarbageCollector* collector) {
  while (!IsEmpty()) {
    mirror::FinalizerReference* ref = DequeuePendingReference()->AsFinalizerReference();
    mirror::HeapReference<mirror::Object>* referent_addr = ref->GetReferentReferenceAddr();
    if (referent_addr->AsMirrorPtr() != nullptr &&
        !collector->IsMarkedHeapReference(referent_addr)) {
      mirror::Object* forward_address = collector->MarkObject(referent_addr->AsMirrorPtr());
      // If the referent is non-null the reference must queuable.
      DCHECK(ref->IsEnqueuable());
      // Move the updated referent to the zombie field.
      if (Runtime::Current()->IsActiveTransaction()) {
        ref->SetZombie<true>(forward_address);
        ref->ClearReferent<true>();
      } else {
        ref->SetZombie<false>(forward_address);
        ref->ClearReferent<false>();
      }
      cleared_references->EnqueueReference(ref);
    }
  }
}

void ReferenceQueue::ForwardSoftReferences(MarkObjectVisitor* visitor) {
  if (UNLIKELY(IsEmpty())) {
    return;
  }
  mirror::Reference* const head = list_;
  mirror::Reference* ref = head;
  do {
    mirror::HeapReference<mirror::Object>* referent_addr = ref->GetReferentReferenceAddr();
    if (referent_addr->AsMirrorPtr() != nullptr) {
      visitor->MarkHeapReference(referent_addr);
    }
    ref = ref->GetPendingNext();
  } while (LIKELY(ref != head));
}

void ReferenceQueue::UpdateRoots(IsMarkedVisitor* visitor) {
  if (list_ != nullptr) {
    list_ = down_cast<mirror::Reference*>(visitor->IsMarked(list_));
  }
}

}  // namespace gc
}  // namespace art
