#pragma once

#include "book/types.hpp"
#include "itch/messages.hpp"

namespace book {

// Applies ITCH order messages to any book with the add/execute/cancel/remove/replace interface.
// Trade (P), cross (Q) and broken trade (B) messages never change the displayed book, so there
// are no overloads for them.
template <class Book>
class FeedHandler {
 public:
  explicit FeedHandler(Book& book) noexcept : book_(&book) {}

  void on(const itch::AddOrder& m) {
    book_->add(m.locate(), m.ref(), side_from_itch(m.side()), m.shares(), m.price());
  }
  void on(const itch::AddOrderMpid& m) {
    book_->add(m.locate(), m.ref(), side_from_itch(m.side()), m.shares(), m.price());
  }
  void on(const itch::OrderExecuted& m) { book_->execute(m.ref(), m.shares()); }
  // The execution price can differ from the order's, but the shares leave the resting price.
  void on(const itch::OrderExecutedWithPrice& m) { book_->execute(m.ref(), m.shares()); }
  void on(const itch::OrderCancel& m) { book_->cancel(m.ref(), m.shares()); }
  void on(const itch::OrderDelete& m) { book_->remove(m.ref()); }
  // Replace carries no side or symbol; the book takes both from the original order.
  void on(const itch::OrderReplace& m) {
    book_->replace(m.old_ref(), m.new_ref(), m.shares(), m.price());
  }

 private:
  Book* book_;
};

constexpr bool is_book_event(char type) noexcept {
  switch (type) {
    case itch::AddOrder::kType:
    case itch::AddOrderMpid::kType:
    case itch::OrderExecuted::kType:
    case itch::OrderExecutedWithPrice::kType:
    case itch::OrderCancel::kType:
    case itch::OrderDelete::kType:
    case itch::OrderReplace::kType:
      return true;
    default:
      return false;
  }
}

}  // namespace book
