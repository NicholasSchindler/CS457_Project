## 1. Setup

Feeding Claude the info it needs to get started, as well as placing strict boundaries so that I can closely monitor the process.

---

### 1.1. Prompt 

We are building a cmd based game for a class. The purpose of the project is to guide AI through writing a network-based application.

I will be manually reviewing all code. Verify with me before running any command line actions or editing code. When editing code, ensure I approve all adds / deletes before you commit. Verify entire program works as intended after each commit.

###  1.1. My Action

Verify Claude is configured correctly.

---

### 1.2. Prompt

Review the contents of directory **blank**. The _docs_ directory contains the plan for the game we are going to build. Review and describe your understanding.

### 1.2. My Action

Verify that Claude understands my planned game.

---

### 1.3. Prompt

Below are the assignment instructions, including rubric. Review and describe your understanding.

### 1.3. My Action

Verify that Claude understands the assignment.

---

### 1.4. Prompt

The coding standard is as follows:
* Language will be C
* Follow the Google style guide
* Use descriptive names over compact names
* Utilize modular code as often as practical

### 1.4. My Action

Verify that Claude understands and utilizes this standard.

---

## 2. Implementation

For all implementation steps, my action will be to verify what Claude wrote and that it did not do more or less than I asked. I will be reviewing all code before it is committed and testing per prompt 1.1.
The goal is to break the coding into bite-size pieces that I can review and understand.

---

### 2.1. Prompt

We are going to walk through the program step-by-step using the mermaid diagram in _docs/fsm_specification.md_. First, as setup, create classes that will generate the JSON messages for the server and clients.

---

### 2.2. Prompt

Write server and client code to complete 'INIT' thru 'WAITING_FOR_MOVES', incuding 'P1 disconnects'.

---

### 2.3. Prompt

Write server and client code for the path that starts with 'Player sends bad message' thru 'WAITING_FOR_MOVES'.

---

### 2.4 Prompt

Write server and client code for the path that starts with 'Either player disconnects' thru 'Reset' and 'WAITING FOR PLAYERS'.

---

### 2.5 Prompt

Write server and client code for the path 'Both players send move message' thru 'Server sends STATUS_UPDATE' and 'WAITING_FOR_MOVES'.

---

### 2.6 Prompt

Writer server and client code for 'One player wins' thru 'GAME_OVER'.

---

### 2.7 My Action

At this point, I plan to review the code and verify that it looks like everything is in place and complete. Part of this, since the
Mermaid Diagram is relatively simple, will be manual path testing.

---

### 2.8 Prompt

I've tested and everything looks complete. Do you see anything that was missed?

