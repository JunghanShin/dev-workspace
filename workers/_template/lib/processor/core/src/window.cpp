#include "processor/window.hpp"

#include <algorithm>

namespace processor {

Window::Window(int size) { set_size(size); }

void Window::set_size(int size) {
    if (size < 2 || size > 10000) throw ConfigError("window must be 2..10000");
    size_ = size;
    while (values_.size() > static_cast<size_t>(size_)) {
        sum_ -= values_.front();
        values_.pop_front();
    }
}

void Window::add(double v) {
    values_.push_back(v);
    sum_ += v;
    ++total_;
    if (values_.size() > static_cast<size_t>(size_)) {
        sum_ -= values_.front();
        values_.pop_front();
    }
}

void Window::reset() {
    values_.clear();
    sum_ = 0;
}

double Window::avg() const { return values_.empty() ? 0.0 : sum_ / static_cast<double>(values_.size()); }
double Window::min() const { return values_.empty() ? 0.0 : *std::min_element(values_.begin(), values_.end()); }
double Window::max() const { return values_.empty() ? 0.0 : *std::max_element(values_.begin(), values_.end()); }

}  // namespace processor
