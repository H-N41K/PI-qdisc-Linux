# PI queue disc implementation in the Linux Kernel

This repository hosts the implementation of the Proportional Integral (PI) controller queue discipline within the Linux Kernel. The PI controller serves as a queuing discipline, offering an alternative to RED (Random Early Detection). The code presented here has been adapted for Linux Kernel version 6.2.0-36-generic, originating from Suraj Singh, Gurupungav Narayanan, and Adwaith Gautham's Linux PI code for Linux Kernel version 5.0.0+, available at https://github.com/linux-pi-programmers/


# Steps to patch the PI qdisc in the Linux Kernel

1. **Install Prerequisites:**
   ```bash
   $ sudo apt install bison flex libssl-dev libelf-dev pkg-config libbpf-dev libcap-dev libmnl-dev
   ```

2. **Enable Source Code Download**:

    ```bash
    $ sudo software-properties-gtk
    ```

    Then, tick the "Source code" checkbox in the "Ubuntu Software" tab and update sources lists:

    ```bash
    $ sudo apt-get update
    ```
3. **Obtain Kernel Source Code and Headers:**
    ```bash
    $ cd ~/Desktop
    $ sudo apt-get source linux
    $ sudo apt-get install linux-headers-$(uname -r)
    ```

4. **Set Permissions of Source Code Directory:**
    ```bash
    $ sudo chmod 777 ~/Desktop/linux-<KERNEL_VERSION> -R
    ```
    KERNEL_VERSION is the kernel version, e.g., "6.2.0-36-generic" in my case.

5. **Edit pkt_sched.h**

    Edit file ‘/usr/src/linux-headers-<KERNEL_VERSION>/include/uapi/linux/pkt_sched.h’ to add enums and a struct for interprocess communication via netlink. The code snippet to be added is as follows:

    ```C
    // PI
    enum {
        TCA_PI_UNSPEC,
        TCA_PI_QREF,
        TCA_PI_LIMIT,
        TCA_PI_W,
        TCA_PI_A,
        TCA_PI_B,
        TCA_PI_ECN,
        TCA_PI_BYTEMODE,
        __TCA_PI_MAX
    };

    struct tc_pi_xstats {
        __u64 prob;
        __u32 qlen;
        __u32 packets_in;
        __u32 overlimit;
        __u32 maxq;
        __u32 dropped;
        __u32 ecn_mark;
    };

    #define TCA_PI_MAX   (__TCA_PI_MAX - 1)	
    ```

6. **Copy sch_pi.c**

    Copy sch_pi.c from this repository to  ‘~/Desktop/linux-<KERNEL_VERSION>/net/sched/’ directory.

7. **Edit Makefile**

    Edit file ‘~/Desktop/linux-<KERNEL_VERSION>/net/sched/Makefile’ and add the following line below the line that reads ‘CONFIG_NET_SCH_PIE’.

    ```bash
    obj-m += sch_pi.o
    ```

    This modification ensures that sch_pi is built as a loadable kernel module.

8. **Copy Kernel Configuration and Prepare**

    ```bash
    $ cd ~/Desktop/linux-<KERNEL_VERSION>
    $ cp /boot/config-$(uname -r) .config
    $ make oldconfig
    $ make prepare
    $ make scripts
    $ cp -v /usr/src/linux-headers-$(uname -r)/Module.symvers .
    ```
	
    This sequence ensures that the kernel configuration is copied, checked for compatibility, and the source code is prepared for building the modules. The module.symvers file is also copied as is crucial for maintaining symbol versioning consistency when loading a module.

9. **Compile and install kernel modules in net/sched**

    ```bash
    $ cd net/sched
    $ make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
    $ sudo make -C /lib/modules/$(uname -r)/build M=$(pwd) modules_install
    $ depmod -a
    ```

    This sequence compiles and builds the kernel modules in net/sched directory. It also generates a module dependency file.

10. **Clone iproute2**

    ```bash
    cd ~/Desktop && git clone https://github.com/iproute2/iproute2.git
    ```

11. **Edit pkt_sched.h in iproute2**
	
	Edit file `~/Desktop/iproute2/include/uapi/linux/pkt_sched.h` and add the same code snippet as mentioned in step 5.

12. **Copy q_pi.c in iproute2**
	Copy q_pi.c from this repository to ‘~/Desktop/iproute2/tc/’ directory.

13. **Edit Makefile in iproute2**

    Look for the line that reads ‘TC_MODULES += q_pie.o’. Below that line, add the following line:

    ```bash
    TC_MODULES += q_pi.o
    ```
    
    This will ensure that the q_pi.o module is included as part of the build process.

14. **Compile and Install iproute2**

    ```bash
    $ cd ~/Desktop/iproute2
    $ make
    $ sudo make install    
    ```

15. **Verify installation of PI module**

    ```bash
    $ tc qdisc add dev <INTERFACE_NAME> root pi
    $ tc qdisc show
    ```
    <INTERFACE\_NAME> is the name of the interface where you wish to add the PI qdisc.

    If you see an entry for the pi qdisc then the module is successfully loaded.

## References

- C. V. Hollot, Vishal Misra, Don Towsley and Wei-BoGong, "On Designing Improved Controllers for AQM Routers Supporting TCP Flows", IEEE/INFOCOM, 2001. Available online at ftp://gaia.cs.umass.edu/pub/MisraInfocom01-AQM-Controller.pdf

- Suraj Singh, Gurupungav Narayanan, and Adwaith Gautham. Linux PI Programmers
GitHub Organization. 2019 https://github.com/linux-pi-programmers/

- https://askubuntu.com/questions/436247/uris-for-linux-image-3-11-0-15-generic

- https://github.com/Xilinx/dma_ip_drivers/pull/44#issuecomment-584447315

