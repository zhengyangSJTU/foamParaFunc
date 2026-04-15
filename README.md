# foamParaFunc

`foamParaFunc` is a C++ command-line utility for running OpenFOAM functions over multiple time directories with resource-aware parallel scheduling.

It is designed for batch post-processing and reconstruction workflows on HPC systems, where manually running commands one time step at a time is inefficient.

---

## 1. What this program does

`foamParaFunc` assumes that the current shell environment has already loaded OpenFOAM correctly.

In other words, commands such as:

- `foamListTimes`
- `postProcess`
- `reconstructPar`

must already be available in the current environment.

The current implementation has been tested successfully with **OpenFOAM 10**.

The program works by:

1. Reading the target OpenFOAM command from `-func`
2. Detecting all available time steps using `foamListTimes`
3. Filtering time steps according to `-time`
4. Launching one task per time step
5. Measuring the memory footprint of a pilot task
6. Dynamically limiting concurrency according to:
   - user-requested parallelism (`-n`)
   - available CPU slots
   - available memory
7. Writing each task output into a separate log file
8. Stopping the whole workflow if any task fails

This makes it suitable for large-scale OpenFOAM post-processing or reconstruction jobs on shared compute nodes.

---

## 2. Implementation principle and command-line arguments

### 2.1 General command format

```bash
foamParaFunc -func "COMMAND [ARGS...]" [-time TIME_SPEC] [-n N] [-timeSource auto|case|processor]
```

Correct form:

```bash
foamParaFunc -func "postProcess -func Q" -time "0.902:1.4" -n 8
```

Another example:

```bash
foamParaFunc -func "reconstructPar -fileHandler collated" -n 64
```

The program appends the selected time step automatically when submitting each task.

---

### 2.2 `-func`

`-func` is **required**.

It specifies the OpenFOAM command to run, including any additional arguments needed by that command.

Examples:

```bash
foamParaFunc -func "postProcess"
foamParaFunc -func "postProcess -func yPlus"
foamParaFunc -func "reconstructPar -fileHandler collated"
```

#### How it works

If you run:

```bash
foamParaFunc -func "postProcess" -time "0.901:0.903" -n 64
```

then the program internally submits tasks like:

```bash
postProcess -time 0.901
postProcess -time 0.902
postProcess -time 0.903
```

### 2.3 `-time`

`-time` is optional.

It controls which time steps are included in the task list.

#### Supported forms

**1. Not provided**

If `-time` is omitted, all detected time steps are processed.

Example:

```bash
foamParaFunc -func "postProcess" -n 8
```

**2. Single time value**

Only one time step is processed.

Example:

```bash
foamParaFunc -func "postProcess" -time 0.902 -n 1
```

This means only time step `0.902` is processed.

**3. Time range**

A closed interval is accepted in the form:

```bash
-time "start:end"
```

Example:

```bash
foamParaFunc -func "postProcess" -time "0.902:1.4" -n 8
```

This means all detected time steps in the range `[0.902, 1.4]` are included, sorted in ascending numerical order.

The program will process them as:

```text
0.902, 0.903, 0.904, ..., 1.4
```

---

### 2.4 `-n`

`-n` is optional. Default value: `1`

It specifies the **maximum requested parallelism**.

Example:

```bash
foamParaFunc -func "postProcess" -time "0.902:1.4" -n 8
```

This means the user requests up to 8 concurrent tasks.

#### Actual parallelism rule

The actual concurrency used by the program is determined dynamically as:

```text
actual_parallelism = min(user_requested_n, current_cpu_limit, current_memory_limit)
```

So even if you specify:

```bash
-n 64
```

it does **not** mean 64 tasks will always run at the same time.

The program will limit concurrency according to:

- current CPU availability
- current available memory
- measured memory usage of tasks

This is intended to avoid oversubscription on shared HPC nodes.

---

### 2.5 `-timeSource`

`-timeSource` is optional. Default value: `auto`

It controls where time steps are detected from.

Available options:

#### `-timeSource auto`
Automatically choose the time source based on the command in `-func`.

Current behavior:

- `postProcess` → uses `case`
- `reconstructPar` → uses `processor`
- `reconstructParMesh` → uses `processor`

This is the recommended default mode.

#### `-timeSource case`
Use time steps from the current case directory.

Equivalent to calling:

```bash
foamListTimes
```

This is suitable for commands such as `postProcess`, when time directories already exist in the case root.

#### `-timeSource processor`
Use time steps from processor directories.

Equivalent to calling:

```bash
foamListTimes -processor
```

This is suitable for reconstruction commands such as `reconstructPar`, where the root case may not yet contain reconstructed time directories.

Example:

```bash
foamParaFunc -func "reconstructPar -fileHandler collated" -timeSource processor -n 64
```

---

## 3. Compilation and installation

### 3.1 Compile

Compile the program with:

```bash
g++ -std=c++17 -O2 -pthread foamParaFunc.cpp -o foamParaFunc
```

---

### 3.2 Install into a personal executable path

A convenient approach is to place the executable in `$HOME/.local/bin`.

```bash
mkdir -p $HOME/.local/bin
mv ./foamParaFunc $HOME/.local/bin/
```

Then make sure `$HOME/.local/bin` is in your `PATH`.

Reload the shell configuration:

```bash
source ~/.bashrc
```

After that, you should be able to run:

```bash
foamParaFunc -func "postProcess" -time "0.901:0.903" -n 64
```

from any location where the executable is available in your environment.

---

## 4. Tested examples

At the moment, the following two workflows have been tested.

### 4.1 Post-processing example

Command:

```bash
foamParaFunc -func "postProcess" -time "0.901:0.903" -n 64
```

Example output:

```text
func: postProcess
time source: case (auto)
total time steps: 3
requested n: 64
cpu hard limit: 64
cpu available now: 63
logs: myParaFunc_logs_20260415_225356
[1/3] submitted: time=0.901
                 time=0.901 finished
pilot peak memory: 5.41 GB
memory-limited parallelism: 17
actual parallelism now: 17
[2/3] submitted: time=0.902
[3/3] submitted: time=0.903
                 time=0.902 finished
                 time=0.903 finished
```

This shows that:

- the program detected 3 time steps
- the time source was automatically set to `case`
- a pilot task was run first
- memory usage was estimated from the pilot task
- subsequent parallelism was limited dynamically

---

### 4.2 Reconstruction example

Command:

```bash
foamParaFunc -func "reconstructPar -fileHandler collated" -n 64
```

Example output:

```text
func: reconstructPar -fileHandler collated
time source: processor (auto)
total time steps: 76
requested n: 64
cpu hard limit: 64
cpu available now: 46
logs: myParaFunc_logs_20260415_224728
[1/76] submitted: time=0.001
                   time=0.001 finished
pilot peak memory: 52.00 MB
memory-limited parallelism: 1163
actual parallelism now: 46
[2/76] submitted: time=0.002
[3/76] submitted: time=0.003
[4/76] submitted: time=0.004
[5/76] submitted: time=0.005
```

This shows that:

- the program automatically selected `processor` as the time source
- time steps were detected from processor directories
- the memory requirement for reconstruction was much lower than in the post-processing example
- the actual parallelism was limited by currently available CPU slots rather than memory

---

## 5. Notes and limitations

### 5.1 Do not put `-time` inside `-func`

Do **not** write commands such as:

```bash
foamParaFunc -func "postProcess -time 0.901"
```

because `foamParaFunc` appends the per-task `-time` argument automatically.

Use this instead:

```bash
foamParaFunc -func "postProcess" -time 0.901
```

---

### 5.2 The OpenFOAM environment must already be loaded

This program does not load OpenFOAM by itself.

You must first source your OpenFOAM environment, for example according to your local cluster setup.

---

### 5.3 The command in `-func` must support `-time`

This scheduler works by launching one task per time step.

Therefore, it is intended for OpenFOAM commands that can be run independently on individual time directories.

If a command does not support `-time`, or does not behave correctly when split into independent time-step tasks, then `foamParaFunc` is not the right launcher for that workflow.

---

### 5.4 Current testing status

So far, the program has been tested with:

- `postProcess`
- `reconstructPar -fileHandler collated`

Additional commands may work as long as they are compatible with the per-time-step execution model.
