
## Overview

These notes do **not** explain how to configure, compile, package, install, or boot a custom Linux kernel.

Instead, this is a simple source code edit you can make while learning the kernel build process. It can be especially useful when attempting to build a custom kernel for the first time.

The change adds a custom message during the early kernel boot process. After building, installing, and booting the custom kernel, you can search for the message in the kernel ring buffer.

Finding the message provides a simple way to confirm that:

- You modified the kernel source code.
- Your change was included in the kernel build.
- The newly built kernel was installed.
- The system booted into your custom kernel.
- Your custom code was executed during kernel initialization.

⚠️ 🛑 **IMPORTANT NOTE: Perform this only on a test system or in a controlled lab environment.** 🛑 ⚠️ 

## Create a Kernel Source Branch

Before modifying the source code, create a new Git branch from the kernel version you want to build.

For example:

```
git checkout -b private-custom-build-demo kernel-6.12.0-55.9.1.el10_0
```

Confirm the current branch:

```
git branch --show-current
```

Example:

```
private-custom-build-demo
```

You can also confirm the kernel source commit from which the branch was created:

```
git show HEAD | head
```

Example:

```
[...]
[...]
[...]

    [redhat] kernel-6.12.0-55.9.1.el10_0
```

## Locate the Early Kernel Boot Code

The file we will modify is:

```
init/main.c
```

This file contains much of the main Linux kernel initialization sequence.

The `start_kernel()` function begins many of the kernel's core startup operations, including early CPU, memory, architecture, security, and system initialization.

It also prints the Linux kernel banner that appears near the beginning of the kernel boot log.

Open the file with your preferred editor:

```
vim init/main.c
```

Search for the following line inside `start_kernel()`:

```
pr_notice("%s", linux_banner);
```

One way to locate the surrounding code is:

```
grep 'boot_cpu_init' init/main.c -A3
```

Example output:

```
	boot_cpu_init();
	page_address_init();
	pr_notice("%s", linux_banner);
	setup_arch(&command_line);
```

## Understanding `linux_banner`

The `linux_banner` variable contains the Linux kernel version and build information printed near the beginning of the boot process.

The message may look similar to:

```
[    0.000000] Linux version 6.12.0-55.9.1.el10_0.x86_64 (mockbuild@...........
```

Depending on the build, the banner may also contain the build user, build host, compiler version, and build number.

Because the Linux banner is printed very early during boot, adding our message immediately after it makes the custom message easy to locate in the kernel log.

## Understanding `pr_notice()`

The Linux kernel provides logging macros such as: `pr_notice()`


A macro is a C preprocessor definition that acts as a shortcut for another piece of code.

Before the source code is compiled, the C preprocessor expands the macro into the code it represents.

The `pr_notice()` macro is defined in:

```
include/linux/printk.h
```

Its definition looks like:

```
/**
 * pr_notice - Print a notice-level message
 * @fmt: format string
 * @...: arguments for the format string
 *
 * This macro expands to a printk with KERN_NOTICE loglevel. It uses pr_fmt() to
 * generate the format string.
 */
#define pr_notice(fmt, ...) \
        printk(KERN_NOTICE pr_fmt(fmt), ##__VA_ARGS__)
```

The macro passes the provided message to the kernel logging system using the `KERN_NOTICE` log level.

Messages sent through the kernel logging system are recorded in the kernel ring buffer and can later be viewed with tools such as `dmesg`.

## Add the Custom Boot Message

Add a new `pr_notice()` line immediately after the existing Linux banner. Here we will add:

```
pr_notice("Alice: Curiouser and curiouser!\n");
```


Example below:

```
	boot_cpu_init();
	page_address_init();
	pr_notice("%s", linux_banner);
	pr_notice("Alice: Curiouser and curiouser!\n");
	setup_arch(&command_line);
```



The `\n` at the end adds a newline so the next kernel message begins on a separate line.

The modified section of `start_kernel()` should look similar to below:

```
void start_kernel(void)
{
        char *command_line;
        char *after_dashes;

        set_task_stack_end_magic(&init_task);
        smp_setup_processor_id();
        debug_objects_early_init();
        init_vmlinux_build_id();

        cgroup_init_early();

        local_irq_disable();
        early_boot_irqs_disabled = true;

        /*
         * Interrupts are still disabled. Do necessary setups, then
         * enable them.
         */
        boot_cpu_init();
        page_address_init();
        pr_notice("%s", linux_banner);
	    pr_notice("Alice: Curiouser and curiouser!\n");
        setup_arch(&command_line);
        /* Static keys and static calls are needed by LSMs */
        jump_label_init();
        static_call_init();
        early_security_init();
        setup_boot_config();
        setup_command_line(command_line);
        setup_nr_cpu_ids();
        setup_per_cpu_areas();
        smp_prepare_boot_cpu(); /* arch-specific boot-cpu hooks */
        early_numa_node_init();
        boot_cpu_hotplug_init();


```

## Confirm the Source Code Change

Confirm that the new line appears in the expected location:

```
grep 'boot_cpu_init' init/main.c -A4
```

Expected output:

```
	boot_cpu_init();
	page_address_init();
	pr_notice("%s", linux_banner);
	pr_notice("Alice: Curiouser and curiouser!\n");
	setup_arch(&command_line);
```

Review the `git diff`:

```
git diff
```

Example:

```
$ git diff
diff --git a/init/main.c b/init/main.c
index 3c57bfac4b9c..06281589e4af 100644
--- a/init/main.c
+++ b/init/main.c
@@ -922,6 +922,7 @@ void start_kernel(void)
 	boot_cpu_init();
 	page_address_init();
 	pr_notice("%s", linux_banner);
+	pr_notice("Alice: Curiouser and curiouser!\n");
 	setup_arch(&command_line);
```

## Commit the Change

Stage the modified file:

```
git add init/main.c
```

Commit the change:

```
git commit -m "Add custom kernel boot message"
```

The kernel can now be built and installed using the kernel build process you are currently learning or testing.

## Validate the Custom Kernel

After building and installing the kernel, boot the test system into the newly built kernel.

Confirm the running kernel version:

```
uname -r
```

Search the kernel ring buffer for the custom message:

```
dmesg | grep 'Alice'
```

Expected output:

```
[    0.000000] Alice: Curiouser and curiouser!
```

The timestamp near `0.000000` (0 seconds) shows that the message was recorded very early during the kernel boot process.

Finding the message confirms that the running kernel contains the source code change that was added before the kernel was built.

## What This Validates

This simple test helps confirm that:

- The kernel source was successfully modified.
- The modified source was included in the kernel build.
- The newly built kernel was installed.
- The system booted into the custom kernel.
- The custom message executed during early kernel initialization.

This does not validate every part of the kernel build or installation process, but it provides a simple and visible first test for anyone who is still in the early stages of learning how to modify and build a custom Linux kernel.

