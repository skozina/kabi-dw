## Synopsis

The aim of **kabi-dw** is to detect any changes in the ABI between the successive builds of the Linux kernel.
This is done by dumping the DWARF type information (the .debug\_info section) for the specific symbols into the text files and later comparing the text files.

## Example

Build your kernel with CONFIG\_DEBUG\_INFO set:

```
git clone git://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git
cd linux && git checkout v4.5
make allyesconfig
make
```

Get all the modules *and* vmlinux into single directory

```
make modules_install && cp vmlinux /usr/lib/modules/4.5.0
```

... build another kernel you want to compare to, lets say v4.6.

Build a list of symbols you want to monitor:

~~~
cat > symbols << EOF
init_task
schedule
dev_queue_xmit
__kmalloc
printk
EOF
~~~

Generate the type information for both kernels based on the current whitelist

~~~
./kabi-dw generate -s symbols -o kabi-4.5 /usr/lib/modules/4.5.0
./kabi-dw generate -s symbols -o kabi-4.6 /usr/lib/modules/4.6.0
~~~

Compare the two type dumps:

~~~
./kabi-dw compare kabi-4.5 kabi-4.6
~~~

## Motivation

Traditionaly Unix System V had a stable ABI to allow external modules to work with the OS kernel without a recompilation called Device Driver Interface.
Linux however never developed such stable kernel ABI. Therefore it's vital to monitor all kernel interfaces used by the external module for change, and if such change happens, the module needs to be recompiled.

Linux has an option (CONFIG\_MODVERSIONS) to generate a checksum identifing all exported symbols thourhg the EXPORT\_SYMBOL() macro. But these checksum are not sufficient to actually identify the scope of the change. For example changing a couple of unused padding bits in a structure to a new field won't break any external modules, but such change changes the chekcsum of any function which receives such structure through its arguments.

## Installation

This program needs elfutils installed. Check out your distribution to figure out how to install elfutils.

For *Fedora* and *CentOS 7* systems:
~~~
dnf install kabi-dw
~~~

For *Ubuntu* systems, you need to compile it. Install the dependencies:
~~~
sudo apt-get install elfutils
~~~

Then just make and run:
~~~
make
./kabi-dw
~~~

## Contributors

Developed by Stanislav Kozina, Red Hat, Inc. with the help of others.

## License

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
