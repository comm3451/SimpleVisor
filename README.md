# SimpleVisor (AMD64 / SVM edition)

This is an **AMD-only** fork of [SimpleVisor][simplevisor], ported from Intel
VT-x to AMD-V (SVM) and trimmed down to build as a single Windows kernel driver
in Visual Studio 2022. It keeps the original project's goal of being the
smallest, clearest possible reference hypervisor: it hyperjacks the running
Windows system into a guest, virtualizes it from within the host, and can be
unloaded again at runtime. The Intel VT-x and UEFI code paths from upstream
have been removed, so the whole thing is one small NT driver.

> **Status: untested reference code.** The SVM engine was written against the
> AMD64 Architecture Programmer's Manual, Volume 2, but has not yet been
> compiled with the WDK or run on hardware. Treat it as a starting point that
> must be validated (and very likely debugged) on a real AMD machine or under
> nested virtualization. A misconfigured VMCB on AMD surfaces as a
> `VMEXIT_INVALID`, which the exit handler turns into a clean load failure
> rather than a crash, but on-hardware validation is still required. For a
> battle-tested AMD reference in the same spirit, compare against Satoshi
> Tanda's [SimpleSvm][simplesvm].

## What changed from upstream

* The Intel VMX engine (`vmx.h`, `shvvmx.c`, `shvvmxhv.c`, `shvvmxhvx64.asm`)
  and the segment/MSR helper (`shvutil.c`) were replaced with an AMD SVM engine
  (`svm.h`, `shvsvm.c`, `shvsvmhv.c`, `shvsvmx64.asm`).
* The UEFI target was dropped. The project is now a single `NT|x64`
  configuration.
* EPT/VPID (Intel second-level address translation) is gone. This port does
  **not** enable AMD Nested Page Tables (NPT); the guest keeps its own page
  tables and guest-physical maps straight through to system-physical, which is
  all a live-system hyperjack needs. NPT is an obvious next extension.
* The OS layer (`shv.c`, `shvvp.c`, `nt/shvos.c`, `nt/shvosx64.asm`) and the
  load / unload skeleton are kept from upstream, largely unchanged.

## How it works

The lifecycle mirrors the original SimpleVisor, with SVM in place of VMX:

1. `ShvLoad` broadcasts a Generic DPC to every logical processor.
2. On each processor, the current register state is captured with
   `RtlCaptureContext`. SVM is enabled (`EFER.SVME`), a host state-save area is
   programmed (`VM_HSAVE_PA`), and a guest VMCB is built from the captured
   state.
3. `VMSAVE` snapshots the live FS/GS/TR/LDTR and SYSCALL/SYSENTER MSRs into the
   VMCB; the remaining guest state (ES/CS/SS/DS, control registers, GDTR/IDTR,
   RIP/RSP/RFLAGS) is filled in by hand.
4. The processor switches to a private hypervisor stack and runs the guest in a
   `VMRUN` loop. The guest resumes at `ShvVpRestoreAfterLaunch`, which sets the
   Alignment Check flag and returns into the loader; that flag is how the
   loader learns virtualization is now live.
5. Unlike Intel, an AMD VM exit returns inline after `VMRUN`, so the host
   preserves the guest general purpose registers around `VMRUN` in a small
   assembly routine and re-enters the guest after servicing each exit.

Only two intercepts are enabled: **CPUID** (used to advertise the hypervisor
present bit, answer the interface query with the SimpleVisor signature, and
carry the ring-0 magic unload request) and **VMRUN** (which AMD requires to be
intercepted for `VMRUN` to be legal). Unloading is requested through the same
CPUID back-channel the original uses: the handler returns the per-VP data
pointer, disables SVM, and resumes the now un-hyperjacked guest transparently.

Like upstream, assembly is kept to a minimum: a stack switch into the exit
loop, the `VMRUN` register round-trip, and a `VMSAVE` helper. The SVM opcodes
are byte-encoded so any version of `ml64` assembles them.

## Building

This edition is built with **Visual Studio 2022** and the matching Windows
Driver Kit (WDK). Install the "Desktop development with C++" workload, the
Windows SDK, and the WDK (plus its Visual Studio extension) so the
`WindowsKernelModeDriver10.0` platform toolset is available. Open `shv.sln`,
select the `NT|x64` configuration, and build. Keep the existing compiler and
linker settings (`nt/nt.props`, `nt/nt.default.props`) as they are.

## Requirements to run

* An AMD (or compatible) x64 processor with SVM support
  (`CPUID Fn8000_0001_ECX[SVM]`), and SVM not disabled/locked by firmware (some
  BIOSes hide it behind an "SVM Mode" or "AMD-V" option).
* 64-bit Windows. x86 Windows is not supported.
* No other hypervisor already owning SVM. If Hyper-V, Virtualization Based
  Security / Memory Integrity, WSL2, or a Credential Guard stack is active, the
  firmware/OS hypervisor holds SVM and this driver will not load. Disable them
  (or test in a VM configured for nested SVM) first.

Because virtualizing a live Windows system from a driver is inherently risky,
validate on a dedicated test machine or a snapshotted VM, with a kernel
debugger attached, before trusting it anywhere else.

## Installation on Windows

Because x64 Windows requires all drivers to be signed, you must testsign the
driver. The Visual Studio project can do this via the "Driver Signing" options
by enabling "Test Sign" with your own certificate (which the UI can generate
for you).

Secondly, you must enable Test Signing Mode on the target machine. To do so,
first turn off "Secure Boot" in firmware, otherwise Test Signing mode cannot be
enabled. Alternatively, if you possess a valid KMCS certificate, you may
"Production Sign" the driver to avoid this requirement.

To enable Test Signing Mode:

```
bcdedit /set testsigning on
```

After a reboot, create the Service Control Manager entry:

```
sc create simplevisor type= kernel binPath= "<PATH_TO_SIMPLEVISOR.SYS>"
```

Then load and unload the hypervisor with:

```
sc start simplevisor
sc stop simplevisor
```

You must have administrative rights for these commands.

## Caveats

SimpleVisor is designed to minimize code size and complexity, which comes at
the cost of robustness. Many SVM operations "should" never fail, but memory
corruption, CPU errata, invalid host state, firmware quirks, and plain bugs can
all cause failures that this code does not check for. ***It does no meaningful
error checking, validation, or exception handling. It is not production
software; it is a reference code base.*** On top of that, this AMD port has not
been run on hardware yet (see the status note at the top).

## Credits and references

* Original SimpleVisor (Intel VT-x) by Alex Ionescu:
  <https://github.com/ionescu007/SimpleVisor>
* SimpleSvm, a mature AMD-V reference in the same spirit, by Satoshi Tanda:
  <https://github.com/tandasat/SimpleSvm>
* AMD64 Architecture Programmer's Manual, Volume 2 (System Programming),
  chapter 15 (SVM) and Appendix B (VMCB layout).
* HyperPlatform by Satoshi Tanda:
  <https://github.com/tandasat/HyperPlatform>
* Bareflank hypervisor SDK: <https://github.com/Bareflank/hypervisor>

[simplevisor]: https://github.com/ionescu007/SimpleVisor
[simplesvm]: https://github.com/tandasat/SimpleSvm

## License

This port is distributed under the same terms as the original SimpleVisor.

```
Copyright 2016 Alex Ionescu. All rights reserved. 

Redistribution and use in source and binary forms, with or without modification, are permitted provided
that the following conditions are met: 
1. Redistributions of source code must retain the above copyright notice, this list of conditions and
   the following disclaimer. 
2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions
   and the following disclaimer in the documentation and/or other materials provided with the 
   distribution. 

THIS SOFTWARE IS PROVIDED BY ALEX IONESCU ``AS IS'' AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL ALEX IONESCU
OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

The views and conclusions contained in the software and documentation are those of the authors and
should not be interpreted as representing official policies, either expressed or implied, of Alex Ionescu.
```
