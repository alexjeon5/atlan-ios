// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/demo.hpp"
#include <iostream>
int main() {
    const auto result = atlan::run_demo();
    std::cout << "ARM32 result=" << result.value
              << " event=" << atlan::event_name(result.execution.event)
              << " retired=" << result.execution.retired << '\n';
    return result.value == 42 && result.execution.event == atlan::Event::supervisor_call ? 0 : 1;
}
