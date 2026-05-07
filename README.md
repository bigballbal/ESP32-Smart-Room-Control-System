
This project is a smart room control system implemented in C.

It simulates an embedded system with manual and automatic control modes.
The system can adjust a virtual air conditioner based on temperature thresholds and allows manual control of devices such as lights and AC.

The goal of this project is to practice embedded system architecture design, state management, and layered software structure (input, control, output).

------------------------------Features-----------------------------------

- Manual mode control for light and AC
- Automatic temperature-based AC control
- State-based system (Manual / Auto mode)
- Command-driven user interface
- Logging system for system events and warnings

-----------------------System Architecture-------------------------------
-
Input Layer → Control Layer → Output Layer

Input Layer:
- User command input
- Temperature input

Control Layer:
- Mode switching (Manual / Auto)
- Auto temperature control logic
- System logging

Output Layer:
- System status display
- -------------------------Key Design Concepts----------------------------

- - State-based design using enums (Manual / Auto mode)
- Modular function decomposition (separation of control logic and I/O)
- Simple logging system for debugging and system tracing
- Threshold-based decision making for automatic control
