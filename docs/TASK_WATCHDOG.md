# Supervision of liveness by task

A hung control loop must lead to the outputs being cut before the time the safety file requires. This is the logic of that supervision as data and code that run on the host. **Nothing here has run on a board and the real watchdog was never frozen**; the bench row of the safety file (`ISO-P05`, freeze the control loop and check the cut before the required time) is the check that remains.

`tools/task_watchdog.py` is the reference and `src/mcu_stm32h757/CM4/task_watchdog.h` its C twin; `tests/test_task_watchdog.py` and `tests/test_task_watchdog.c` hold both to the same vectors (`tests/task_watchdog_vectors.h`, written by `tools/gen_taskwd_vectors.py`).

| Rule | What the supervisor does |
|---|---|
| Each task checks in when it finishes a pass | a check-in of a number that is no task is ignored: it is never a sign of life |
| The supervisor looks every `update_period_ms` and hands the hardware watchdog its refresh only while no mandatory task is late | one refresh for the whole system, not one per task |
| A mandatory task late by more than its deadline is a hang | the stop is asked for once and the supervisor latches: it never refreshes again, even if the hung task wakes up |
| A task that is not mandatory is late | only a degraded mark; it recovers when the task checks in |

The worst time from a hang to the cut is the longest mandatory deadline plus one supervisor period plus the timeout of the hardware watchdog: 105 ms with the values of `config/task_watchdog.cfg`, which holds the tasks, their deadlines and the time the safety file requires (150 ms) and is refused when the worst time is longer than the required one. A simulation freezes the control task at every phase of its period and checks the bound. The caller builds the arrays for `TaskWd_Init` from the file and sends the stop (a TRIP of `docs/ESTOP_CAN.md`) when `trips` grows; which tasks exist in the firmware and where they check in is not decided here.
