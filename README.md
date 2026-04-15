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

### 2.2 `-time`

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

---

### 2.3 `-n`

`-n` is optional. Default value: `1`

It specifies the **maximum requested parallelism**.

The program will limit concurrency according to:

- current CPU availability
- current available memory
- measured memory usage of tasks

This is intended to avoid oversubscription on shared HPC nodes.

---

### 2.4 `-timeSource`

`-timeSource` is optional. Default value: `auto`

It controls where time steps are detected from.

Available options:

#### `-timeSource auto`
Automatically choose the time source based on the command in `-func`.

Current behavior:

- `postProcess` → uses `case`
- `reconstructPar` → uses `processor`
- `reconstructParMesh` → uses `processor`
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

## 4. Notes and limitations

### 4.1 Do not put `-time` inside `-func`

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

### 4.2 The OpenFOAM environment must already be loaded

This program does not load OpenFOAM by itself.

You must first source your OpenFOAM environment, for example according to your local cluster setup.

---

### 4.3 The command in `-func` must support `-time`

This scheduler works by launching one task per time step.

Therefore, it is intended for OpenFOAM commands that can be run independently on individual time directories.

If a command does not support `-time`, or does not behave correctly when split into independent time-step tasks, then `foamParaFunc` is not the right launcher for that workflow.

---

### 4.4 Current testing status

So far, the program has been tested with:

- `postProcess`
- `reconstructPar -fileHandler collated`

Additional commands may work as long as they are compatible with the per-time-step execution model.
