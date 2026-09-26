# Debugging SimpleVisor (AMD-SVM edition)

A beginner's guide to **building**, setting up a safe environment for, loading, and debugging `simplevisor.sys` — the AMD-V / SVM "hyperjacking" hypervisor in this repository. It assumes you have never set up kernel debugging and do not yet know how a hypervisor works. Every step is spelled out, and there is a [glossary](#glossary-terms-used-in-this-guide) for the jargon.

> ⚠️ **SAFETY WARNING — READ THIS FIRST.** This driver takes over the CPU's virtualization engine on **every** logical processor. A bug can **instantly reboot or hard-freeze the whole machine with no blue screen and no way to recover in software** — you lose unsaved work and risk filesystem corruption. **Do all testing in a snapshotted virtual machine** (or a throwaway spare AMD PC), **never on your main computer.** Take a snapshot before every load.

---

## What this driver actually is (30-second orientation)

You do not need to understand hypervisors to follow this guide, but this mental picture helps everything else make sense:

- A normal driver runs *inside* Windows. This driver is different: when it loads, it turns the **entire already-running Windows into a "guest"** and inserts a tiny **"host"** that runs *beside* Windows. This is called *hyperjacking*. It does **not** boot a new operating system.
- It is a fork of Alex Ionescu's SimpleVisor, ported from Intel VT-x to **AMD-V (SVM)**. It is **AMD-only** — none of this works on an Intel CPU.
- You load and unload it on demand as a Windows service. Loading hyperjacks the machine; unloading cleanly reverses it. It can be unloaded at runtime.
- This is **untested reference code.** The repository's own README says the SVM engine "has not yet been compiled with the WDK or run on hardware." **Expect that it may not even compile on the first try, and that first-run load failures are normal, not the exception.** That is exactly why the whole guide is built around a disposable, snapshotted target, and why there is a fallback plan for build errors ([§0.5](#05-if-it-does-not-compile)).

---

## Glossary (terms used in this guide)

Skim this once; refer back when a term shows up in a warning.

- **Ring 0 / kernel mode** — the most-privileged CPU mode, where the OS kernel and drivers run. Only ring-0 code can run privileged instructions (this is why the magic "unload" CPUID must be issued from a driver, not from a normal program).
- **User mode / ring 3** — where ordinary `.exe` programs run. No direct hardware access; a normal program cannot issue the unload command.
- **SVM / AMD-V** — AMD's hardware virtualization extension. The feature this driver uses.
- **VMCB (Virtual Machine Control Block)** — the in-memory structure that describes the guest CPU (its registers, segments, control registers) and which events the hypervisor intercepts.
- **VMRUN** — the AMD instruction that runs the guest described by a VMCB.
- **VMEXIT / #VMEXIT** — the CPU pausing the guest and handing control back to the hypervisor (the "host") when an intercepted event happens.
- **GIF (Global Interrupt Flag)** — an AMD flag that, when cleared, blocks *all* interrupts globally. It is cleared automatically on a VMEXIT and re-enabled with the `STGI` instruction. Getting this wrong hangs the CPU.
- **Bugcheck** — Windows' term for the "blue screen of death" (BSOD): a controlled kernel crash that can write a memory dump you can analyze.
- **Triple fault** — when the CPU faults, then faults while handling that fault, then faults again. The hardware gives up and **instantly resets the machine** — no blue screen, no dump.
- **IRQL (Interrupt Request Level)** — a Windows CPU priority level. At high IRQLs, many operations (paging, most kernel APIs, blocking) are forbidden.
- **DISPATCH_LEVEL** — a raised IRQL (level 2) where paging and blocking are illegal. This driver's load/unload run here.
- **DPC (Deferred Procedure Call)** — a short routine Windows runs at DISPATCH_LEVEL. The driver broadcasts its load/unload to every CPU as a DPC.
- **PatchGuard / Kernel Patch Protection (KPP)** — a Windows self-defense feature that bugchecks (code `0x109`) if it detects core kernel structures (like the GDTR/IDTR) left altered.
- **Trap frame** — the saved snapshot of CPU registers at the moment of a fault. WinDbg's `.trap` command switches your view to it.
- **HVCI (Hypervisor-enforced Code Integrity)** — the underlying technology name for the **Memory Integrity / Core Isolation** toggle discussed in [§3](#3-freeing-the-cpu-disable-every-other-hypervisor). When you see "HVCI" and "Memory Integrity," they mean the same thing.

---

## Table of contents

0. [Build the driver](#0-build-the-driver)
1. [A naming note you must read before running any command](#1-a-naming-note-you-must-read-before-running-any-command)
2. [Choosing and preparing a safe target machine](#2-choosing-and-preparing-a-safe-target-machine)
3. [Freeing the CPU: disable every other hypervisor](#3-freeing-the-cpu-disable-every-other-hypervisor)
4. [Test-signing and installing the driver](#4-test-signing-and-installing-the-driver)
5. [Setting up WinDbg kernel debugging](#5-setting-up-windbg-kernel-debugging)
6. [Seeing the driver's debug output](#6-seeing-the-drivers-debug-output)
7. [The hypervisor debugging mindset (this is different!)](#7-the-hypervisor-debugging-mindset-this-is-different)
8. [Reading crashes and troubleshooting](#8-reading-crashes-and-troubleshooting)
9. [First milestones, in order — a checklist](#9-first-milestones-in-order--a-checklist)
10. [Symptom → cause quick-reference table](#10-symptom--cause-quick-reference-table)

---

## 0. Build the driver

You cannot debug a driver you have not built. The repository ships **source code only** — there is no ready-made `.sys`. This section produces one. Do the build on a normal development machine (it does **not** have to be the test machine).

### 0.1 A CPU pre-check before you spend any effort

This driver and everything in this guide are **AMD-only**. Nested AMD-V is impossible on an Intel host, so if your physical machine is Intel, **stop — none of this can work here.** Check the CPU first, in a normal (non-elevated) prompt:

```powershell
Get-CimInstance Win32_Processor | Select-Object Name
```

(Or open **Task Manager → Performance → CPU** and read the name in the top-right.) The name must say **AMD** (Ryzen / EPYC / Threadripper / Athlon, etc.). On older Windows you may also have `wmic cpu get name`, but `wmic` is removed on recent Windows 11 builds, so prefer the PowerShell command above. If it says **Intel**, do not continue.

### 0.2 Install the toolchain (the #1 thing beginners get wrong)

You need **all** of these; installing Visual Studio alone is not enough:

1. **Visual Studio 2022** with the **"Desktop development with C++"** workload (select it in the Visual Studio Installer).
2. The **Windows SDK** (the version bundled with that workload is fine).
3. The **Windows Driver Kit (WDK)** — a separate download that must match your SDK version.
4. **The WDK's Visual Studio extension** (the WDK installer offers to install it at the end; say yes). Without this extension, the **`WindowsKernelModeDriver10.0` platform toolset** and the driver project type are missing, and `shv.sln` **will not open or build.** This missing extension is the most common reason a beginner cannot build the project at all.

### 0.3 Open and build

1. Open `shv.sln` in Visual Studio 2022.
2. In the configuration dropdown, note there is **only one** configuration: **`NT | x64`**. There is **no Debug/Release** choice — this is normal and can be confusing if you are used to ordinary projects. Just leave it on `NT | x64`.
3. **Build → Build Solution** (Ctrl+Shift+B).

A driver (WDM) project builds a **`.sys`**, not a `.exe`.

### 0.4 Find the output and confirm the build worked

- **How you know it worked:** the Visual Studio **Output** window ends with something like `Build: 1 succeeded, 0 failed`.
- **Where the file lands:** the project sets no custom output directory, so the binary appears in the default Visual Studio build folder — typically an **`x64\NT\`** (or `NT\`) subfolder of the solution directory. Do not guess: the **Output** window prints the **full path** to the built `shv.sys` on the "linking" line — copy that path.
- You should find **both** `shv.sys` **and** its matching **`shv.pdb`** (the symbol file) in that folder.

**Write down that full folder path.** You will reuse it for signing ([§4.3](#43-create-a-test-certificate-and-sign-the-sys)), for `binPath=` ([§4.5](#45-install-start-and-stop-the-service-exact-syntax)), and for symbols ([§5.6](#56-load-the-drivers-symbols-only-after-it-is-loaded)).

### 0.5 If it does not compile

This is **untested reference code**; the README explicitly says it has never been compiled with the WDK. A build error here is expected, not a sign you did something wrong. If you hit one:

- Read the error and fix the obvious cause if you can (a mismatched function signature, a missing declaration, etc.).
- Compare the failing area against **Satoshi Tanda's SimpleSvm** (linked in the repo README), a mature, known-working AMD-V reference in the same spirit. It is the best guide to what a correct version of a given routine looks like.
- Treat getting it to *compile* as your true first milestone; getting it to *load* is a later one.

---

## 1. A naming note you must read before running any command

This guide uses the file name **`simplevisor.sys`** and the service name **`simplevisor`** everywhere. But **the project does not build a file with that name by default.**

The Visual Studio project is called `shv` (`shv.sln` / `shv.vcxproj`) and has no output-name override, so a default build produces **`shv.sys`** (as you just saw in [§0.4](#04-find-the-output-and-confirm-the-build-worked)). You have two options — pick one and stick with it:

- **Recommended (keeps every command in this guide literally correct):** rename the build output. In Visual Studio, right-click the `shv` project → **Properties** → **Configuration Properties → General → Target Name** → set it to `simplevisor`. Rebuild. You now get `simplevisor.sys` (and `simplevisor.pdb`).
- **Or substitute:** wherever this guide says `simplevisor.sys`, use `shv.sys` instead. One subtlety: in WinDbg the *module name* is the file name **without the extension**, so it would be `shv` (e.g. `lm m shv`, `x shv!Shv*`, `bp shv!DriverEntry`) rather than `simplevisor`. The Windows *service* name (`simplevisor` in the `sc` commands) is **arbitrary and independent of the file name** — you can keep calling the service `simplevisor` regardless of what the `.sys` is named.

If you are a beginner, do the recommended rename once and forget about it.

---

## 2. Choosing and preparing a safe target machine

> **You already confirmed your CPU is AMD in [§0.1](#01-a-cpu-pre-check-before-you-spend-any-effort).** If you skipped that, do it now — everything below assumes an AMD host.

### 2.1 Why safety is not optional here

When the driver hyperjacks the machine it enables SVM, builds virtualization structures (a VMCB and a nested-page-table identity map), and drops into a host loop that runs with the **Global Interrupt Flag (GIF) cleared** — meaning interrupts are globally off in that context. If anything in that path is wrong, the fault happens with **no operating system underneath it to catch it.** The result is not a readable blue screen; it is typically an **instant reset (triple fault)** or a **dead hang** that needs a hard power-off. Because it hyperjacks **all** logical processors, one bug can wedge every core at once.

Loading is demand-start, so the failure happens the moment you run `sc start simplevisor`, not at boot.

### 2.2 Do **not** test on your main / daily machine

A hypervisor fault reboots or hangs the physical box with no clean shutdown. There is no reliable "unload after a hang": the intended clean unload (`sc stop simplevisor`) works by having ring-0 code issue a special CPUID that the host loop watches for — but if the host loop is already wedged, that channel never runs, so software cannot recover you. Use a target you can **throw away or roll back.**

### 2.3 Recommended path: a Windows VM with nested AMD-V + snapshots

The outer hypervisor (VMware or Hyper-V) contains the fault: a crash inside `simplevisor.sys` resets only *that guest VM*, and you revert to a snapshot in seconds.

**First, you need a guest OS to virtualize.** A total beginner starts with an empty hypervisor and no Windows inside it. Create a VM and install **64-bit Windows** into it *before* you enable nested AMD-V or take any snapshots. A free **Windows Evaluation ISO** from Microsoft's Evaluation Center works well for a throwaway test VM (it expires after a set period, which is fine — you will be reverting snapshots constantly anyway).

Non-negotiable prerequisites (commonly gotten wrong):

- **The physical host CPU must be AMD with SVM ("AMD-V" / "SVM Mode") enabled in the host's UEFI.** You **cannot** get nested AMD-V from an Intel host — a host can only expose the extension its own CPU has.
- The VM must be **powered off** to change CPU/virtualization settings.
- Give the VM **≥ 2 virtual processors.** Bring-up runs per logical processor; a single-vCPU guest hides multiprocessor bugs.
- Guest OS = **64-bit Windows** (a hard requirement of the driver).

**VMware Workstation Pro (use a current 17.x build):**

- Select the VM → **VM > Settings** (Ctrl+D) → **Hardware** tab → **Processors**.
- Under "Virtualization engine", tick the box labeled exactly **"Virtualize Intel VT-x/EPT or AMD-V/RVI"**. On an AMD host this exposes AMD-V (SVM) plus RVI (AMD's nested paging, which this driver's NPT map needs).
- The other two boxes ("Virtualize CPU performance counters", "Virtualize IOMMU") are **not** required — leave them off.
- Set processors × cores per processor so the total is **≥ 2**.
- **Top gotcha:** if the VMware *host's* own Windows has Hyper-V / VBS / Memory Integrity active, VMware cannot pass the extension through, and you get **"Virtualized AMD-V/RVI is not supported on this platform"** (or a silent fallback). Fix it on the **host** using the same disable steps as [§3](#3-freeing-the-cpu-disable-every-other-hypervisor), then reboot the host.

**Hyper-V (AMD host):** run on the **host** in an **elevated** PowerShell, with the VM **off**:

```powershell
Set-VMProcessor -VMName "<VMName>" -ExposeVirtualizationExtensions $true
Set-VMProcessor -VMName "<VMName>" -Count 2
Set-VMMemory    -VMName "<VMName>" -DynamicMemoryEnabled $false
```

- **Dynamic Memory must be OFF for nested virtualization** — the third command above. This is a documented Microsoft requirement: with Dynamic Memory enabled, a nested guest fails to start or misbehaves. The VM therefore runs on **static memory**; set a fixed RAM size that is comfortable for the guest.
- AMD nested virtualization on Hyper-V requires the **host** to run **Windows 11 or Windows Server 2022 (or later)**. Windows 10 / Server 2016 / 2019 support nested virtualization for **Intel only** — AMD will not work there.
- The exact minimum AMD micro-architecture and VM configuration version move over time; **verify against current Microsoft docs rather than trusting a fixed number.** To upgrade the VM config version (VM off): `Update-VMVersion -Name "<VMName>"`, then confirm with `Get-VM -Name "<VMName>" | Format-List Name,Version`.

### 2.4 Why VirtualBox is a poor choice

VirtualBox can expose nested AMD-V (Settings → System → Processor → "Enable Nested VT-x/AMD-V", or `VBoxManage modifyvm "<name>" --nested-hw-virt on`), but for a full hyperjacking hypervisor its nested-SVM support has historically been immature and flaky compared to VMware and Hyper-V, and it conflicts with a Windows host running Hyper-V/VBS. Prefer VMware Workstation or Hyper-V; treat VirtualBox as a last resort and expect problems.

### 2.5 Alternative: a spare physical AMD PC

Valid and in some ways more realistic (real firmware, real nested paging, real timing across real cores), but there is **no snapshot safety net.** If you go this route:

- Requirements: an AMD (SVM-capable) x64 CPU; enable SVM in UEFI (usually under CPU/Advanced, labeled **"SVM Mode"** or **"AMD-V"**, often OFF by default); install **64-bit Windows**; then apply all of [§3](#3-freeing-the-cpu-disable-every-other-hypervisor).
- **Image the OS disk before testing** (Macrium Reflect / Clonezilla / `dd`) so you can restore after a bad run. Keep the backup current — a hard reset can corrupt the filesystem.
- Because the service is **demand-start**, a crash will **not** auto-reload the driver on the next boot — the box comes back clean. Do **not** change the service to boot/auto start.
- Attach a kernel debugger over a physical transport (network, USB2 debug cable, or serial) **before** loading, so a bugcheck or hang is at least observable. A true hang still needs a manual power cycle.

### 2.6 Snapshot discipline — the single most important habit

- **Take a snapshot before *every* `sc start simplevisor`.**
- Best workflow: get the guest into the exact test state — service created, other hypervisors disabled and confirmed (§3), kernel debugger already attached (§5) — then take a **live snapshot that includes RAM**, so reverting lands you right back at the pre-start moment. Then start the driver. Hang → revert → change **one** thing → retry.
  - **VMware:** VM > Snapshot > Take Snapshot; revert via the Snapshot Manager.
  - **Hyper-V:** right-click VM > Checkpoint. Use a **Standard Checkpoint** (full saved state including memory), **not** a Production Checkpoint. Revert = "Apply".
- **Always snapshot at the pre-*start* moment — before the hypervisor is loaded.** Do **not** rely on taking or reverting a snapshot while the driver is loaded (i.e. while the machine is hyperjacked). A RAM snapshot captured with the hypervisor live saves a running nested hypervisor; reverting to it restores that live state, which can behave unpredictably under the outer nested-virtualization layer. Keep every snapshot clean (no SimpleVisor loaded).
- Keep the service demand-start (the default for `sc create ... type= kernel`). A crash then does not re-load the driver on the next boot, so a reverted/rebooted guest comes up clean.

---

## 3. Freeing the CPU: disable every other hypervisor

**This is the number-one reason the driver fails to load.** Enabling nested AMD-V (§2.3) only gives the guest an SVM-capable CPU. If Windows *inside* the guest boots its own Microsoft hypervisor, **that** hypervisor grabs SVM first and hides it from ordinary drivers, so `simplevisor.sys` cannot take it. In that case the driver's SVM probe fails, `ShvLoad` returns failure, `DriverEntry` returns an error, and **`sc start` fails** — with the driver's own line **`The SHV failed to initialize (0xFFFFFFFF) Failed CPU: <n>`** printed alongside it (see [§6.4](#64-good-load-vs-failed-load-and-decoding-the-status)).

The Microsoft hypervisor is pulled in by **any** of: the Hyper-V role, Windows Sandbox, WSL2, Virtual Machine Platform, Windows Hypervisor Platform, VBS / Memory Integrity (Core Isolation), or Credential Guard. (**HVCI**, the "Hypervisor-enforced Code Integrity" acronym you will see in some error text, is just the underlying name for the Memory Integrity / Core Isolation toggle.) **You must disable all of them, not just the Hyper-V role** — disabling only the role while leaving the others on is the most common mistake, because the hypervisor still boots.

Run these in the **guest** (or on the physical target), **elevated**, then **reboot**:

```bat
:: Stop the Windows hypervisor from launching at boot
bcdedit /set hypervisorlaunchtype off
```

Then remove the features that silently re-enable it (elevated PowerShell), and reboot again:

```powershell
Disable-WindowsOptionalFeature -Online -FeatureName Microsoft-Hyper-V-All -NoRestart
Disable-WindowsOptionalFeature -Online -FeatureName VirtualMachinePlatform -NoRestart
Disable-WindowsOptionalFeature -Online -FeatureName HypervisorPlatform -NoRestart
```

- `-NoRestart` stops each command from prompting for its own reboot; reboot once at the end instead.
- **On Windows Home:** the Hyper-V feature is not present, so `Disable-WindowsOptionalFeature ... Microsoft-Hyper-V-All` may **throw a red error** ("feature name … is unknown"). That is fine — there is nothing to disable. Ignore that specific error and **still do** the `bcdedit /set hypervisorlaunchtype off` step and the Memory Integrity step below, because Home can still boot the hypervisor via Memory Integrity.

Then handle the two that are not "optional features":

- **Memory Integrity (Core Isolation):** Windows Security app → Device security → Core isolation → "Core isolation details" → set **Memory integrity = Off** → reboot. (Registry equivalent, *verify before relying on it*: `HKLM\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity`, DWORD `Enabled` = 0.)
- **VBS / Credential Guard:** if enabled, disable via Group Policy ("Turn On Virtualization Based Security" = Disabled) or registry `HKLM\SYSTEM\CurrentControlSet\Control\DeviceGuard`, DWORD `EnableVirtualizationBasedSecurity` = 0. If it is UEFI-locked, removal needs extra steps — **verify the current Microsoft procedure**; the exact steps change.

To re-enable the Windows hypervisor later: `bcdedit /set hypervisorlaunchtype auto`.

**Confirm the hypervisor is really gone — do this every time, after rebooting:**

- Run `msinfo32` → **System Summary**.
- **BAD** (hypervisor still present): the line **"A hypervisor has been detected. Features required for Hyper-V will not be displayed."**
- **GOOD** (clear to load): that line is **absent**; instead you see the four Hyper-V requirement lines ("VM Monitor Mode Extensions", "Second Level Address Translation Extensions", "Virtualization Enabled in Firmware", "Data Execution Prevention") reported **Yes**, and **"Virtualization-based security: Not enabled"**.
- Optional environment pre-check: Sysinternals **Coreinfo** run elevated (`coreinfo -v`) shows whether the CPU exposes SVM/nested paging and whether a hypervisor is already present. The exact row labels vary by Coreinfo build — read them on the machine rather than quoting them. The driver's own failure message remains the authoritative test.

> **Windows 11 note:** 22H2 / 24H2 and many OEM "Secured-core" machines ship **Secure Boot + Memory Integrity ON by default**, and some re-enable VBS via policy after a reboot. Handle all of these, or the driver will not load. **Home** editions cannot install Hyper-V but can still be blocked by Memory Integrity being on.

---

## 4. Test-signing and installing the driver

Windows will not load a self-built, unsigned kernel driver until you (a) turn off Secure Boot, (b) enable test signing, (c) sign the driver with a certificate the machine trusts, and (d) install it as a service. **Every command below needs an elevated (Run as administrator) prompt** — almost every failure in [§4.9](#49-common-install-errors-and-fixes) traces back to a non-elevated shell.

### 4.0 Getting the built files onto the target machine

You built `simplevisor.sys` (and `simplevisor.pdb`) on your dev machine in [§0](#0-build-the-driver). The target is a different machine (usually your test VM). You need to copy these into the target:

- `simplevisor.sys` — to install and run.
- `simplevisor.pdb` — for symbols in WinDbg ([§5.6](#56-load-the-drivers-symbols-only-after-it-is-loaded)).
- `simplevisor.cer` — the exported certificate you will create in [§4.4](#44-make-the-target-machine-trust-the-cert-most-missed-step).

Ways to move files into a VM:

- **VMware / Hyper-V shared folder** (Enhanced Session mode on Hyper-V; Shared Folders on VMware), or simple **drag-and-drop / copy-paste** if VMware Tools / Hyper-V integration is installed.
- **Copy over the network** (a file share, or a USB drive attached to the VM).

> **Re-copy the `.pdb` after *every* rebuild.** WinDbg only resolves symbols when the `.pdb` matches the **exact** build of the loaded `.sys`. If you rebuild the driver, copy the new `.pdb` over too, or symbols will silently fail to load ([§5.6](#56-load-the-drivers-symbols-only-after-it-is-loaded)).

### 4.1 Boot-configuration changes: do them in the right order, reboot as few times as possible

Three of the setup steps change boot configuration and each normally wants a reboot. Doing them in a scattered order means rebooting over and over and losing track. Do them like this:

1. **Turn off Secure Boot in firmware FIRST** ([§4.2](#42-turn-off-secure-boot-first--and-why)). This *must* come before test signing: `bcdedit /set testsigning on` is **rejected** while Secure Boot is on. Changing Secure Boot happens in firmware and involves its own reboot into the UEFI menu.
2. **After Windows comes back, run the `bcdedit` changes together in one elevated prompt**, then reboot **once**:

   ```bat
   bcdedit /set testsigning on
   bcdedit /debug on
   bcdedit /dbgsettings net hostip:<HOST_IP> port:50000
   ```

   (The last two are the kernel-debugging transport — see [§5](#5-setting-up-windbg-kernel-debugging). If you are not attaching a debugger yet, run just the first line.)
3. Reboot once. All three take effect together.

> **BitLocker gotcha (applies to every reboot in this section AND in §5).** If the system drive is BitLocker-encrypted, changing Secure Boot, enabling test signing, or enabling debug **all** trigger a **BitLocker recovery-key prompt** on the next boot. Before touching any of these, **suspend BitLocker** or have the 48-digit recovery key on hand:
>
> ```bat
> manage-bde -protectors -disable C:
> ```

### 4.2 Turn off Secure Boot first — and why

Secure Boot (a UEFI feature) forbids weakening the kernel's code-integrity policy at runtime. Test signing *is* such a weakening, so the kernel refuses to honor it while Secure Boot is on. If you skip this, `bcdedit /set testsigning on` fails with *"The value is protected by Secure Boot policy and cannot be modified or deleted."*

- Turn it off in **firmware**, not in Windows. To reach firmware: Settings → System (Win11) or Update & Security (Win10) → Recovery → Advanced startup → **Restart now** → Troubleshoot → Advanced options → **UEFI Firmware Settings** → Restart. (Shortcut from an elevated prompt: `shutdown /r /fw /t 0`.) In firmware, under Security/Boot, set **Secure Boot = Disabled**, save, exit.
- Verify from Windows afterward: `msinfo32` → **Secure Boot State = Off**, or PowerShell `Confirm-SecureBootUEFI` returns **False** (it throws on legacy-BIOS machines, which just means Secure Boot was never in play).

### 4.3 Confirm test signing is on

After the reboot in [§4.1](#41-boot-configuration-changes-do-them-in-the-right-order-reboot-as-few-times-as-possible), confirm `bcdedit /set testsigning on` took effect:

- A **watermark** appears bottom-right above the clock: "Test Mode … Build …".
- Authoritative check regardless of watermark: run `bcdedit` with no arguments; the current entry lists `testsigning  Yes`.

Turn it back off when done with `bcdedit /set testsigning off` + reboot. Do **not** bother with `bcdedit /set nointegritychecks on` — Windows ignores/resets it on Win8+.

### 4.4 Create a test certificate and sign the `.sys`

Do the certificate creation and signing on the machine where you have the WDK/SDK (usually your dev machine), then carry the signed `.sys` and the exported `.cer` to the target ([§4.0](#40-getting-the-built-files-onto-the-target-machine)).

The tools (`signtool.exe`, `makecert.exe`) ship with the WDK/SDK under `C:\Program Files (x86)\Windows Kits\10\bin\<SDKversion>\x64\`. The easiest place to run them is the **"Developer Command Prompt for VS 2022"**, where they are on the PATH.

**Note:** as committed, the project does not have Driver Signing properties wired up, so the "Visual Studio Test Sign" build option may not be available. The manual command-line route below is the reliable one.

Create a self-signed code-signing certificate (modern PowerShell):

```powershell
New-SelfSignedCertificate -Type CodeSigningCert `
  -Subject "CN=SimpleVisor Test Cert" `
  -CertStoreLocation Cert:\CurrentUser\My -HashAlgorithm SHA256
```

Sign the driver with an embedded signature (elevated Developer Command Prompt). The cert is in the `My` store, so use `/s My`:

```bat
signtool sign /v /fd SHA256 /s My /n "SimpleVisor Test Cert" C:\path\simplevisor.sys
```

- `/fd SHA256` = file digest algorithm. `/n` = a substring of the cert's subject name to select it.
- **Timestamping is optional for test signing.** Only add it if you want the signature to stay valid past the cert's expiry (and it needs network). Use **either** `/t http://timestamp.digicert.com` (Authenticode) **or** `/tr http://timestamp.digicert.com /td SHA256` (RFC3161) — never both. If unsure, omit timestamping entirely.

Sanity-check the signature:

```bat
signtool verify /v /pa C:\path\simplevisor.sys
```

> Use `/pa`, not `/kp`. `signtool verify /kp` checks the *production* kernel policy and can report FAILURE for a self-signed cert even though it loads fine in Test Mode. "Loads in Test Mode" is the real test.

### 4.5 Make the target machine **trust** the cert (most-missed step)

In Test Mode the kernel still requires the signature to chain to a **trusted root installed on that machine.** A cert sitting in `CurrentUser\My` is **not** trusted at driver-load time — even if you built and are testing on the same box.

Export the public certificate:

```powershell
Export-Certificate `
  -Cert (Get-ChildItem Cert:\CurrentUser\My | Where-Object {$_.Subject -eq "CN=SimpleVisor Test Cert"}) `
  -FilePath C:\simplevisor.cer
```

Import it on the **target** machine, **elevated**:

```bat
certutil -addstore Root C:\simplevisor.cer
certutil -addstore TrustedPublisher C:\simplevisor.cer
```

`Root` (Trusted Root Certification Authorities, LocalMachine) is the essential one — it makes the chain valid for load. `TrustedPublisher` suppresses install prompts (belt-and-suspenders).

**Confirm the trust step worked** (this is the step people silently skip, then hit error 577 at load):

```bat
certutil -store Root | findstr /i SimpleVisor
```

This should list your "SimpleVisor Test Cert". If it prints nothing, the cert is not in the machine's Root store and the driver will not load — redo the `certutil -addstore Root` step, elevated, on the target.

### 4.6 Install, start, and stop the service (exact syntax)

Two quirks bite everyone:

1. **In PowerShell, `sc` is an alias for `Set-Content`.** You must type **`sc.exe`**. In `cmd.exe`, plain `sc` is fine — the simplest advice is *use an elevated `cmd.exe`*.
2. **The spaces around `=` are mandatory and asymmetric:** a space is required **after** each `=`, and **no** space before it. `type=kernel` fails; `type= kernel` works. This is the #1 syntax mistake.

```bat
sc create simplevisor type= kernel binPath= "C:\path\simplevisor.sys"
sc start  simplevisor
sc stop   simplevisor
sc delete simplevisor
```

- `binPath=` must be an **absolute** path, **quoted** if it contains spaces. Point it at the copy of the `.sys` **on the target**.
- `type= kernel` marks it a kernel-driver service; the default start type is demand/manual, which is what you want.
- The service name `simplevisor` is arbitrary but must match across create/start/stop/delete. Check state anytime with `sc query simplevisor`.
- **Dev loop after a code change:** `sc stop simplevisor` → rebuild + re-sign → recopy `.sys` **and** `.pdb` to the target → `sc start simplevisor`. No need to `sc delete` unless the `binPath` changes.

### 4.7 What a successful load looks like

On success, `sc start simplevisor` returns cleanly with a RUNNING state, e.g.:

```
SERVICE_NAME: simplevisor
      TYPE   : 1  KERNEL_DRIVER
      STATE  : 4  RUNNING
```

The driver stays resident (it returns success from `DriverEntry` after hyperjacking every CPU). The success debug line **`The SHV has been installed.`** is printed — and with a kernel debugger attached you will see it **by default** ([§6](#6-seeing-the-drivers-debug-output)). An independent functional confirmation is in [§7.4](#74-detection-test-instead-of-breakpoints).

> **Key fact for reading failures:** `DriverEntry` returns the load result to Windows. So **if the hyperjack fails on any CPU, `sc start` itself FAILS** with an error code (e.g. `[SC] StartService FAILED <code>`); it does **not** return RUNNING. "`sc start` succeeded" therefore means *total* success — every CPU hyperjacked. How to read a failed start is [§6.4](#64-good-load-vs-failed-load-and-decoding-the-status).

### 4.8 A note on the dev loop and unload

`sc stop simplevisor` cleanly un-hyperjacks (it uses the ring-0 magic CPUID `0x41414141`/`0x42424242` as its internal control channel) and prints **`The SHV has been uninstalled.`** That message is printed **unconditionally**, even if nothing was actually loaded — so seeing it does not by itself prove a prior successful load.

### 4.9 Common install errors and fixes

| `sc` error | Meaning | Fix |
|---|---|---|
| **577** — `ERROR_INVALID_IMAGE_HASH` ("cannot verify the digital signature") | Signature problem. | In order: confirm `bcdedit` shows `testsigning Yes` **and** you rebooted (§4.3); confirm **Secure Boot is OFF** (§4.2) — this is the classic "I turned test signing on but still get 577"; confirm the `.sys` is signed (`signtool verify /pa`, §4.4); confirm the cert is in **LocalMachine\Root** (`certutil -store Root | findstr /i SimpleVisor`, §4.5). |
| **1275** — `ERROR_DRIVER_BLOCKED` ("This driver has been blocked from loading") | Usually HVCI / Memory Integrity on, and/or the Microsoft Vulnerable Driver Blocklist. | Turn off Memory Integrity and ensure Hyper-V/VBS are fully disabled (§3). A driver-blocklist toggle exists under Windows Security; there is also a registry switch near `HKLM\SYSTEM\CurrentControlSet\Control\CI\Config` — **verify the exact value name for your build before trusting it.** |
| **5** — `ERROR_ACCESS_DENIED` ("Access is denied") | Shell not elevated, or you used bare `sc` in PowerShell. | Run cmd/PowerShell **as Administrator**; use `sc.exe` in PowerShell. |
| **2** — `ERROR_FILE_NOT_FOUND` | Bad/relative `binPath=`, or the `.sys` isn't there. | Use an absolute, quoted path; confirm the file exists on the target (and that you built `simplevisor.sys` — see [§1](#1-a-naming-note-you-must-read-before-running-any-command)). |
| **1072** — "marked for deletion" | `sc delete` while still running or with open handles. | `sc stop` first, close Services.msc, sometimes reboot, then recreate. |
| **any other code, from `sc start`, when the driver's own failure line also appears** | The hyperjack failed inside the driver (a *precondition* problem, e.g. another hypervisor owns SVM), so `DriverEntry` returned failure. | Decode the number (below) and read [§6.4](#64-good-load-vs-failed-load-and-decoding-the-status) / [§8.3](#83-the-three-kinds-of-failure-and-how-to-tell-them-apart). |

**Decoding a failed `sc start` code.** When the failure is *inside* the driver (not a signing/elevation problem), the number `sc start` prints is the driver's status mapped to a Windows error. Decode it at a normal prompt with:

```bat
certutil -error <code>
```

(or in WinDbg, `!error <code>`). It resolves to one of `HV_FEATURE_UNAVAILABLE`, `HV_NO_RESOURCES`, or `HV_NOT_PRESENT`, which maps straight back to the `SHV_STATUS_*` table in [§6.4](#64-good-load-vs-failed-load-and-decoding-the-status). You normally do not even need this, because with a debugger attached the driver's own line **`The SHV failed to initialize (0x…) Failed CPU: <n>`** prints the raw status directly.

> When finished testing, restore security: `bcdedit /set testsigning off` (reboot) and re-enable Secure Boot in firmware. Test signing lets *any* test-signed driver load.

---

## 5. Setting up WinDbg kernel debugging

A kernel debugger halts the **whole** OS, so it cannot run on the same Windows it is debugging. You need **two** Windows environments:

- **TARGET** — the machine that runs `simplevisor.sys` and gets frozen/inspected. Strongly prefer a **snapshotted VM.**
- **HOST** (the debugger machine) — runs WinDbg.

> **You do not need a third computer.** For a VM target, the **HOST is simply the physical machine your VM runs on** — WinDbg runs on the physical box and debugs the guest inside it. There are two viable paths:
> - **(a) Kernel debugger (KD) from the host into the guest** — best for the *risky first loads*, because it receives output synchronously and lets you break in on a hang. This is what §5 sets up.
> - **(b) DebugView inside the single VM** — no debugger at all, just a log viewer inside the guest. Simpler, but only safe **once loading is stable** (it can lose the last messages on a crash). See [§6.5](#65-debugview-single-vm-no-second-machine).
>
> Beginners: use path (a) for bring-up, then switch to (b) for convenience later.

This section only covers **attaching** the debugger. Getting the driver to load (§3, §4) is separate — don't conflate them.

### 5.1 Get WinDbg (on the HOST only)

- **Modern WinDbg** (recommended, best UI): in an elevated PowerShell, `winget install Microsoft.WinDbg`. If that package id fails, run `winget search windbg` and use whatever id it lists, or install "WinDbg" from the Microsoft Store. Launch it from the Start menu as "WinDbg".
- **Classic `windbg.exe`** ships inside "Debugging Tools for Windows" (a feature of the Windows SDK or WDK installer). Default path: `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\windbg.exe` (alongside `kdnet.exe`, `kd.exe`).

You install **no** debugger on the TARGET. **Run WinDbg on the HOST as administrator** (needed to bind the network port and get through the firewall).

### 5.2 Configure the TARGET — network (KDNET) debugging

You already enabled `bcdedit /debug on` and set `dbgsettings` in the consolidated step in [§4.1](#41-boot-configuration-changes-do-them-in-the-right-order-reboot-as-few-times-as-possible). If you skipped it, run these **elevated on the TARGET** now:

```bat
bcdedit /debug on
bcdedit /dbgsettings net hostip:<HOST_IP> port:50000
```

- **`<HOST_IP>` is the IP of the HOST (debugger) machine, not the target.** This is the single most common mistake. Get it on the HOST with `ipconfig`.
- `port:` = any number in 49152–65535; 50000 is fine. Each target talking to one host needs a unique port.
- **The key:** if you omit `key:`, bcdedit **auto-generates and prints** one — copy it verbatim (four dot-separated groups, e.g. `2steg4fzfe4fw.23k259f1lg2b0.1z8gmn0vlnf9v.3fmbl2u4gwvxp`). You need it on the HOST.
- Verify what was written: `bcdedit /dbgsettings` and `bcdedit /enum {current}` (should show `debug Yes`).

> **NIC support caveat:** network kernel debugging requires a NIC on Microsoft's KDNET-supported list. Many physical adapters (and most Wi-Fi) are **not** supported. Among VMs, **Hyper-V Gen-2** works well over KDNET; other hypervisors' virtual NICs are hit-or-miss. To check: copy `kdnet.exe` **and** `VerifiedNICList.xml` (both from the Debuggers folder) onto the target and run `kdnet.exe` with no arguments — it reports whether the NIC is supported. `kdnet.exe <HOST_IP> 50000` can also configure everything and print the key for you.

**Serial-over-named-pipe alternative (best for VMware / VirtualBox, or when the NIC isn't supported):**

```bat
bcdedit /debug on
bcdedit /dbgsettings serial debugport:1 baudrate:115200
```

In VMware VM settings (target powered off): Add Hardware → Serial Port → "Use named pipe" → pipe name `\\.\pipe\com_1` → "This end is the server" → "The other end is an application"; tick **"Yield CPU on poll"**. Named-pipe serial is slower than net but very reliable and needs no supported NIC — it is the safe default for VMware.

### 5.3 Reboot the TARGET

```bat
shutdown /r /t 0
```

Settings only take effect after a reboot. (Same BitLocker recovery-key caveat as [§4.1](#41-boot-configuration-changes-do-them-in-the-right-order-reboot-as-few-times-as-possible).) With net/serial debugging the target boots normally and you attach live afterward — `bcdedit /debug on` only enables the *transport*; it does **not** stall boot waiting for a debugger.

### 5.4 On the HOST — attach and break in

**Modern WinDbg:** `File` → under "Start debugging" → **Attach to kernel**. Pick a tab:

- **Net** tab: Port = `50000`, Key = the key you copied. Optionally tick "Break on connection". OK.
- **Serial** tab (for the pipe alt): Baud rate `115200`, Port `\\.\pipe\com_1`, tick "Pipe" and "Reconnect". OK.

**Classic windbg.exe:** `File` → `Kernel Debug…` (Ctrl+K) → `NET` or `COM` tab with the same values.

**Or from a command line:**

```bat
:: network
windbg -k net:port=50000,key=<w.x.y.z>
:: named-pipe serial
windbg -k com:pipe,port=\\.\pipe\com_1,resets=0,reconnect
```

**Firewall:** on the first net attach, Windows Defender Firewall on the HOST prompts to allow WinDbg — **allow it** (KDNET is UDP). If there is no prompt and it won't connect, manually allow the WinDbg/EngHost executable or open UDP 50000 inbound. This is a frequent "it just won't connect" cause.

**Break in:** after the connection line appears the target is running. Press **Ctrl+Break** (or Debug → Break) to land at a `kd>` prompt.

> **Expectation for a beginner:** breaking in **freezes the entire target OS** (in a VM, the whole guest halts). That is normal, not a crash. Type **`g`** (go) to resume it. Always `g` before you close WinDbg or detach — otherwise the target stays frozen and *looks* hung.

### 5.5 Symbols for Windows itself

Symbols let WinDbg turn raw addresses into function names. Set the Microsoft symbol server now:

```
.sympath srv*C:\symbols*https://msdl.microsoft.com/download/symbols
.reload
```

- `C:\symbols` is a local cache directory (created automatically; any writable path works). The first fetch of Windows symbols can take a while.
- Quick sanity check that symbols and the connection work:

```
vertarget            (prints target Windows version/build + uptime)
lm                   (list modules; you should see nt)
.effmach             (should report x64 — a hard requirement for this driver)
```

### 5.6 Load the driver's symbols — only **after** it is loaded

The driver does not exist in memory until `sc start simplevisor` succeeds ([§4.6](#46-install-start-and-stop-the-service-exact-syntax)). So **do not** run the driver-symbol commands at attach time — you will get "no symbols"/"deferred", and think something is broken. It is not; the module simply isn't loaded yet.

**After** the driver is loaded (milestone M1 in [§9](#9-first-milestones-in-order--a-checklist)), point WinDbg at the driver's `.pdb` and reload:

```
.sympath+ C:\path\to\driver\pdb_folder
.reload /f simplevisor.sys
```

- Put the driver's **`.pdb`** (built next to `simplevisor.sys`, [§0.4](#04-find-the-output-and-confirm-the-build-worked)) in the sympath — **not the `.sys`**. The `.pdb` must match the exact build of the loaded `.sys` (rebuild → recopy the `.pdb`).
- Verify: `lm m simplevisor` should show a real `.pdb` path (not "no symbols"/"deferred"), and `x simplevisor!Shv*` should list functions like `ShvLoad`, `ShvUnload`. (If you kept the default file name, use `shv` here — see [§1](#1-a-naming-note-you-must-read-before-running-any-command).)

If you want to catch the driver at the instant it loads (to breakpoint `DriverEntry`), see the `sxe ld` recipe in [§7.4(d)](#74-detection-test-instead-of-breakpoints) — that arms the break *before* `sc start`, then loads symbols the moment the image appears.

---

## 6. Seeing the driver's debug output

The driver prints its status via `vDbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, ...)`. The three messages you will see, **all emitted at ERROR level**, are:

- **`The SHV has been installed.`** — success (from `ShvLoad`, on `sc start`).
- **`The SHV has been uninstalled.`** — from `ShvUnload` (on `sc stop`; printed unconditionally).
- **`The SHV failed to initialize (0x%lX) Failed CPU: %d`** — failure (from `ShvLoad`).

### 6.1 With a kernel debugger, these lines show **by default**

Here is the good news that a lot of older guides get wrong: **ERROR-level** kernel messages are displayed in an attached kernel debugger **by default**, with no mask configuration at all. Windows combines each component's filter mask with the default mask, and a component mask that was never set still lets **bit 0 — `DPFLTR_ERROR_LEVEL`** — through. Since all three of this driver's messages are ERROR-level, **a KD session shows them out of the box.** Just `sc start` / `sc stop` and watch the WinDbg command window.

You only need to *raise* a mask to reveal higher-verbosity output — WARNING / TRACE / INFO levels. **This driver emits none of those**, so for SimpleVisor the mask step below is essentially unnecessary in a KD session. It is documented here only for completeness and for the registry (no-symbols / DebugView) case.

The driver's component is `IHVDRIVER`. Raising the component-specific mask (`IHVDRIVER`) or the catch-all `DEFAULT` mask to `0xF` reveals *all* levels for that component — a safe superset if you ever add higher-verbosity prints.

### 6.2 Raising the mask in WinDbg (live, no reboot) — optional for this driver

Requires a live kernel debugger (§5) with symbols resolved (§5.5):

```
ed nt!Kd_IHVDRIVER_Mask 0xF        (this driver only)
ed nt!Kd_DEFAULT_Mask   0xF        (or reveal everything)
dd nt!Kd_IHVDRIVER_Mask L1         (verify it reads back as 0000000f)
```

Use `ed` (enter DWORD), not `eq`. If WinDbg says *"Couldn't resolve error at 'nt!Kd_IHVDRIVER_Mask'"*, fix symbols first: `.symfix` then `.reload /f nt`. To dump lines already printed into the kernel's small circular buffer, use `!dbgprint`.

### 6.3 Setting the mask via the registry (persistent, no symbols needed)

Only relevant for DebugView-only setups or machines without symbol-server access, and again **only if you later add non-error prints** — the existing ERROR-level lines do not need it. Run elevated, then **reboot** (this mask is read at boot):

```bat
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Debug Print Filter" /v IHVDRIVER /t REG_DWORD /d 0xF /f
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Debug Print Filter" /v DEFAULT   /t REG_DWORD /d 0xF /f
```

The value name is the component name with **no** `Kd_` prefix and **no** `_Mask` suffix (so `Kd_IHVDRIVER_Mask` → value `IHVDRIVER`).

### 6.4 Good load vs failed load, and decoding the status

**Good load:** `sc start` returns RUNNING, and the debugger shows exactly one line — `The SHV has been installed.` It is printed **once**, from `ShvLoad`, only after **all** logical processors report success.

**Failed load:** `DriverEntry` returns the failure to Windows, so **`sc start` itself fails** with an error code ([§4.7](#47-what-a-successful-load-looks-like), [§4.9](#49-common-install-errors-and-fixes)). Alongside that service error, the driver prints its own line to the debugger (shown by default, per [§6.1](#61-with-a-kernel-debugger-these-lines-show-by-default)):

`The SHV failed to initialize (0x<HEX>) Failed CPU: <n>`

where `<n>` is the zero-based CPU that failed. So a failed load normally gives you **both** signals: the `sc start` error *and* the driver's failure line. The status is a small negative number printed as unsigned 32-bit, so decode it like this:

| Printed hex | Code | Meaning |
|---|---|---|
| `0xFFFFFFFF` | `-1` `SHV_STATUS_NOT_AVAILABLE` | The SVM feature probe failed: SVM not present, disabled in firmware (`VM_CR.SVMDIS`), or the SVM CPUID bit is hidden because **another hypervisor already owns SVM**. The most common real-world cause: you missed something in [§3](#3-freeing-the-cpu-disable-every-other-hypervisor). |
| `0xFFFFFFFE` | `-2` `SHV_STATUS_NO_RESOURCES` | Allocation of per-CPU data failed (out of memory / no contiguous block). |
| `0xFFFFFFFD` | `-3` `SHV_STATUS_NOT_PRESENT` | `VMRUN` executed but **immediately produced a `#VMEXIT` with exit code `VMEXIT_INVALID`** because the processor's VMCB consistency checks failed — so the guest never actually ran. The exit handler cleanly disables SVM and resumes; the loader's "is our hypervisor present?" self-check then finds nothing and reports this. See [§8.3 case B](#83-the-three-kinds-of-failure-and-how-to-tell-them-apart). |

The `sc start` error itself decodes to the matching NTSTATUS/HV error (`STATUS_HV_FEATURE_UNAVAILABLE` / `STATUS_HV_NO_RESOURCES` / `STATUS_HV_NOT_PRESENT`); decode any such code with `certutil -error <code>` at a normal prompt or `!error <code>` in WinDbg rather than memorizing hex.

> **The one case with a driver line but *no* `sc` error:** the driver re-installs itself on resume-from-sleep via a power callback, and **ignores** the result. So if a hyperjack fails *on resume* (not on `sc start`), you get the `The SHV failed to initialize (…)` line with **no** accompanying service error — because the service was already started earlier. This is the only situation that produces "a failure line with no `sc` error." Reinforces the advice in [§7.5](#75-extra-hazards-specific-to-this-driver): don't sleep/hibernate the target.

---

### 6.5 DebugView (single VM, no second machine)

Sysinternals **DebugView** (`Dbgview.exe`) lets you watch output on **one** box without WinDbg. Download it from Microsoft/Sysinternals and run it **inside the test VM, elevated** (kernel capture loads a helper driver and needs admin). In the **Capture** menu:

- **Capture Kernel** (Ctrl+K) — turns on kernel `DbgPrint` capture. With this on, DebugView captures this driver's **ERROR-level** lines (all three of them) — you do **not** need anything more for SimpleVisor.
- **Enable Verbose Kernel Output** — only affects *higher-verbosity* (WARNING/TRACE/INFO) messages, which this driver does not emit. Leaving it on is harmless (belt-and-suspenders), but it is **not required** to see SimpleVisor's messages.
- **Capture Events** (Ctrl+E) — the global on/off; make sure it's on.

Then `sc start simplevisor` / `sc stop simplevisor` and watch the window.

**DebugView's limit — and when it is not enough:** it is a user-mode viewer that *buffers* kernel output. If the box **hangs, bugchecks, or resets before output is flushed, you lose those lines** — you may never see the message that mattered. A kernel debugger receives output synchronously and lets you break in on a hang. **For the risky first loads, prefer the debugger; use DebugView once loading is stable.**

> **Mutually exclusive:** do **not** run DebugView's "Capture Kernel" while a kernel debugger is attached. Kernel `DbgPrint` is routed to the debugger, and DebugView will receive nothing. Use one or the other for kernel output.

---

## 7. The hypervisor debugging mindset (this is different!)

Debugging this driver is **not** like debugging a normal driver, and using normal techniques in the wrong place will crash the machine. Read this section before you set your first breakpoint.

### 7.1 Two worlds after a successful hyperjack

Once `sc start` succeeds there are two execution contexts:

- **The guest** — the entire running Windows. Your kernel debugger (§5) attaches to and debugs **this**, exactly as before, because the hypervisor is nearly transparent. Only two things are intercepted (CPUID and the mandatory VMRUN); debug exceptions, breakpoints, and the debugger transport are **not** intercepted, so **all normal debugger features keep working in the guest.**
- **The host** — the hypervisor's `#VMEXIT` handler and VMRUN loop (in `shvsvmhv.c` and `shvsvmx64.asm`). This runs *beside* the guest, on its **own stack**, with the **Global Interrupt Flag (GIF) cleared.** The debugger has **no transport into this world** — it is a guest facility, and the guest is not executing while the CPU is in host context.

### 7.2 Why you cannot breakpoint the exit handler

A source breakpoint (`bp`) set in the exit handler, the CPUID handler, the VMEXIT loop, or anywhere in the assembly VMRUN region **will not fire as a usable break** — that code is run by the *host*, which is not the guest your debugger controls. Worse: if such a breakpoint somehow *did* execute in host context, the `int3` would try to call the debugger engine with **GIF cleared, on a foreign stack** — undefined and effectively catastrophic (hang / triple-fault / instant reset, no blue screen). **Treat "breakpoint in the exit handler" as a way to kill the machine, not a debugging tool. Never single-step the exit handler.**

### 7.3 Why you must not add `DbgPrint` inside the exit handler

The exit handler runs at arbitrary/unknown IRQL with GIF cleared, outside normal OS scheduling. `DbgPrint`/`vDbgPrintEx` and essentially every kernel API assume normal context and may take locks or touch pageable memory — from host context they can **deadlock or fault**, again with no clean blue screen. The exit handler is deliberately minimal and must stay that way. **The safe `DbgPrint` calls that already exist are all on the guest/edge side** (in `ShvLoad`/`ShvUnload`) — those are your "print on the edges."

### 7.4 Techniques that actually work

**(a) Incremental bring-up** — verify one layer before stacking the next, and add instrumentation *only* on the guest/edge side, **one change at a time.** The natural order: detection works → SVM enables and the first VMRUN round-trip completes → CPUID interception is live → the system keeps running normally (nested paging is transparent) → clean unload returns control. The single message **`The SHV has been installed.`** actually proves most of this chain end-to-end at once.

**(b) Detection test instead of breakpoints — the primary "is it live?" check.** Run CPUID from a tiny user-mode program and check two things:

```c
#include <stdio.h>
#include <intrin.h>

int main(void)
{
    int r[4];

    __cpuid(r, 1);                        // leaf 1
    int hvPresent = (r[2] >> 31) & 1;     // ECX bit 31 = "hypervisor present"

    __cpuidex(r, 0x40000001, 0);          // hypervisor interface leaf
    int isShv = (r[0] == 0x20766853);     // EAX == 'Shv ' (bytes 'S','h','v',' ')

    printf("hypervisor-present bit : %d\n", hvPresent);
    printf("SimpleVisor signature  : %d\n", isShv);
    printf("live SimpleVisor       : %s\n",
           (hvPresent && isShv) ? "YES" : "no");
    return 0;
}
```

**How to build and run it** (this is an ordinary user-mode program, *not* a driver — no signing, no admin, no test mode needed):

1. Save it as `det.c`.
2. Open the **"Developer Command Prompt for VS 2022"** (so `cl` is on the PATH).
3. Compile: `cl det.c`
4. Run: `det.exe`

Interpretation: **live SimpleVisor ⇔ `hypervisor-present bit == 1` AND `SimpleVisor signature == 1`.**

> **Commonly gotten wrong:** ECX bit 31 of leaf 1 is set by **any** hypervisor (Hyper-V, VMware, VirtualBox…), so it does **not** prove *this* driver is loaded — especially inside a VM, whose outer hypervisor already sets it. The **definitive** test is `CPUID(0x40000001).EAX == 0x20766853`. And do **not** put the `0x41414141`/`0x42424242` magic CPUID in this program — that is the **unload** command and only works when issued from **ring 0** (kernel mode); from a normal user-mode `.exe` it just runs a harmless ordinary CPUID and unloads nothing. Use it only to unload, from the driver.

**(c) Post-mortem state — how to "see inside" the host without breakpoints.** Have the exit handler **stash** values into ordinary non-paged memory it already has mapped (a per-CPU field, or a small global ring buffer) — writing to mapped RAM is safe from host context, while calling APIs is not — then read that memory from WinDbg **after** breaking into the guest or after unload. The richest artifact is the VMCB: `GuestVmcb.ControlArea.ExitCode` tells you *why* the last exit happened (`0x72` = CPUID, expected; `-1` / `VMEXIT_INVALID` = the CPU rejected the VMCB), and `GuestVmcb.StateSaveArea` holds the guest RIP/RSP/RFLAGS/CRn.

> **Gap to fill for post-mortem work (advanced — see the safety note in [§7.6](#76-safety-when-adding-your-own-instrumentation)):** as written there is no global pointer to the per-CPU `SHV_VP_DATA` (each is allocated per CPU, and the pointer is only recoverable at runtime through the unload path). For debugging, add a global array of `PSHV_VP_DATA` indexed by CPU (populate it in `ShvVpLoadCallback` right after allocation) and/or a global ring buffer, so WinDbg can find them. Then, since you build the driver and have full private symbols:
>
> ```
> dt simplevisor!_SHV_VP_DATA <addr>
> dt simplevisor!_SHV_VP_DATA <addr> GuestVmcb.ControlArea.ExitCode GuestVmcb.StateSaveArea.Rip
> dq <ringbuffer-addr> L40
> ```

**(d) Normal WinDbg breakpoints ARE valid and useful everywhere *outside* the exit handler** — i.e. in guest/driver context, especially **before** the first VMRUN. Safe breakpoint targets: `DriverEntry`, `ShvLoad`, `ShvVpLoadCallback`, `ShvVpInitialize`, `ShvSvmProbe`, `ShvSvmSetupVmcb`, `ShvSvmNptInitialize`, `ShvVpRestoreAfterLaunch` (this is the guest re-entry point), `ShvUnload`, `ShvVpUnloadCallback`. The driver isn't present until `sc start`, so break on image load first, then symbol-load, then set function breakpoints:

```
sxe ld simplevisor.sys        ; break when the image is mapped (before DriverEntry)
g
; ---- breaks on module load ----
.reload /f simplevisor.sys
bp simplevisor!DriverEntry
bp simplevisor!ShvLoad
bp simplevisor!ShvVpInitialize
g
```

(If `sxe ld simplevisor.sys` is rejected, try `sxe ld:simplevisor`.) Useful helpers: `lm m simplevisor`, `x simplevisor!Shv*`, and `~` to list/switch processors (load runs on all CPUs).

**How to actually inspect state once a breakpoint hits** (a beginner reflex to build):

- To read a function's **return value**, let it run to its return and read `AL`/`EAX`/`RAX` (x64 returns integers there): `gu` (go up — runs to the caller) then `r al` (or `r eax`). A boolean like `ShvSvmProbe` returns non-zero in `AL` for TRUE.
- To inspect the **structure being built**, e.g. after `bp simplevisor!ShvSvmSetupVmcb` hits, find the `PSHV_VP_DATA` argument (on x64 the first argument arrives in `rcx`: `r rcx`) and dump it: `dt simplevisor!_SHV_VP_DATA @rcx`.
- To see **where you are** and the stack: `r` (registers), `k` (call stack), `u .` (disassemble at the current instruction).

### 7.5 Extra hazards specific to this driver

- **Partial-load hazard (real in this code):** load broadcasts to **every** CPU nearly simultaneously (via `KeGenericCallDpc` at DISPATCH_LEVEL). If some CPUs succeed and one fails, `ShvLoad` returns failure, `DriverEntry` returns error, and Windows **unloads the driver image** — leaving the succeeded CPUs still hyperjacked while their host code was just freed → hang/reset. So **"load reported failure" does not guarantee a clean machine.** Reboot/revert after any failed load.
- **DPC watchdog:** because load runs inside a `KeGenericCallDpc` at DISPATCH_LEVEL, **lingering too long at a load-path breakpoint** can trip `CLOCK_WATCHDOG_TIMEOUT` (0x101) or `DPC_WATCHDOG_VIOLATION` (0x133). Don't dawdle at those breakpoints.
- **Power transitions re-trigger it:** the driver re-installs on resume from sleep (via a power callback that **ignores the result** — see the note in [§6.4](#64-good-load-vs-failed-load-and-decoding-the-status)) and uninstalls on suspend. **Sleeping/hibernating the target will re-run the hyperjack** and can surprise you mid-debug — prefer not to sleep the target; use snapshots.

### 7.6 Safety when adding your own instrumentation

The post-mortem work in [§7.4(c)](#74-detection-test-instead-of-breakpoints) and milestone **M7** in [§9](#9-first-milestones-in-order--a-checklist) ask you to **edit the hypervisor's source.** For an audience new to hypervisors, editing this driver — especially anything reachable from the host / exit path in `shvsvmhv.c` — is itself a way to introduce a machine-killing bug. Treat it as **advanced and optional**, and follow these rules:

- **Only write values into already-mapped non-paged memory** (a per-CPU field or a preallocated global buffer). **Never** add a `DbgPrint` or any other kernel API call in the exit handler or CPUID handler ([§7.3](#73-why-you-must-not-add-dbgprint-inside-the-exit-handler)).
- Make **one** change at a time.
- After each rebuild: **re-sign** the `.sys`, **recopy** both `.sys` and `.pdb` to the target, and **take a fresh snapshot** before loading.

---

## 8. Reading crashes and troubleshooting

### 8.1 Configure the target to write a crash dump (do this before the first test)

Run `sysdm.cpl` → **Advanced** → **Startup and Recovery → Settings**:

- **Write debugging information:** choose **"Kernel memory dump"** (usually enough) or **"Complete memory dump"** (best for a hypervisor, which can touch arbitrary physical memory — the nested-page map covers 512 GB, so bugs can be anywhere).
- **Uncheck "Automatically restart"** so a bugcheck stays on screen instead of rebooting past it.
- Default dump path: `%SystemRoot%\MEMORY.DMP`.

"Complete memory dump" needs a pagefile on the system volume ≥ (RAM + 1 MB) or a dedicated dump file; if the option is hidden, force it via `HKLM\SYSTEM\CurrentControlSet\Control\CrashControl` → `CrashDumpEnabled` = 1 plus a large `DedicatedDumpFile`. **With a KD attached, a bugcheck breaks *into the debugger first* rather than silently writing a dump** — analyze live, and save one yourself with `.dump /f C:\mem.dmp` if you want it.

### 8.2 Commands to run when it breaks in

```
!analyze -v          ; primary: bugcheck code + args, faulting module, stack, and (with private symbols) the source line
.bugcheck            ; just the bugcheck code + args
k / kb / kv          ; call stacks (kv shows a trap-frame address at a fault)
.trap <addr>         ; switch register context to the fault point (from kv), then read RIP and the faulting instruction
lmvm simplevisor     ; confirm the module loaded and symbols match
.reload /f simplevisor.sys
ln <addr> / u <addr> ; nearest symbol / disassemble around an address
!dbgprint            ; recover the driver's debug messages from the in-memory buffer (works even from a dump)
```

For source-line mode: `.symfix`, `.sympath+ <build dir>`, `.srcpath+ <src dir>`, `.reload`, `.lines`.

### 8.3 The three kinds of failure, and how to tell them apart

**This distinction dictates your whole method**, because only the first kind gives you anything to analyze:

- **CLEAN BUGCHECK (Windows caught it):** a blue screen, a dump, and a stack. `!analyze -v` and `k` work. Happens when the driver faults in **normal/guest** context — before VMRUN, or in guest code paths after a successful hyperjack.
- **INSTANT REBOOT, NO DUMP = a TRIPLE FAULT.** The CPU reset before Windows could bugcheck. **Important nuance for this driver:** a *grossly* misconfigured VMCB (bad segment attributes, bad control registers, out-of-range RIP, bad GDTR/IDTR) is exactly what `VMRUN`'s **consistency checks catch and report as `VMEXIT_INVALID`** — and this driver turns that into the **clean, reported `0xFFFFFFFD` load failure** (case B below / [§6.4](#64-good-load-vs-failed-load-and-decoding-the-status)), *not* a crash. So most bad-VMCB conditions do **not** triple-fault here. A triple fault instead requires guest state that **passes** `VMRUN`'s checks yet is still unrunnable — e.g. a plausible-but-wrong RIP or segment base that faults on the *first guest instruction* and then faults again, or a broken nested-page (NPT) map, or a host-state / GIF ordering bug. `!analyze` is useless (nothing was written). **This is exactly why snapshots matter** — you cannot catch it live; you revert and bisect.
- **DEAD HANG, NO DUMP, NO REBOOT.** Common cause here: GIF left cleared / interrupts globally masked (nothing schedules, the clock stops), or the host loop spinning without guest progress. In KD: Break in, then `!running` and `~*k` (stacks of all processors) and `!locks`. **Caveat:** if a CPU is wedged in host context with GIF cleared, KD may be unable to break *that* CPU — another reason snapshots + incremental bring-up beat trying to debug a wedged hypervisor live.

**Rule of thumb:** only the clean-bugcheck class yields `!analyze -v`. For the other two, the real tools are **revert the snapshot, add edge prints, and bisect what you enabled** (SVM enable → VMCB build → VMRUN → intercepts → unload).

**Mapping the two "clean failure" statuses to a stage** (remember: these come with a failed `sc start`, [§6.4](#64-good-load-vs-failed-load-and-decoding-the-status)):

- **`sc start` fails and prints `0xFFFFFFFF`** → detection failed: SVM feature bit absent, firmware-disabled, or another hypervisor owns SVM. Check Task Manager → Performance → CPU → "Virtualization" (Disabled → enable SVM in BIOS; Enabled but failing → something owns it), and re-run the [§3](#3-freeing-the-cpu-disable-every-other-hypervisor) confirmation via `msinfo32`.
- **`sc start` fails and prints `0xFFFFFFFD` (case B):** `VMRUN` executed but the very first exit came back as `VMEXIT_INVALID` — the processor's consistency checks rejected the VMCB, so the guest never ran, and the driver turned that into a clean, reported failure instead of a crash. Next step: set a breakpoint **before** the launch (e.g. `bp simplevisor!ShvSvmLaunchOnVp`) and inspect the VMCB build in guest context — segment attributes, CR0/CR3/CR4, EFER (with SVME), GDTR/IDTR base+limit, guest RIP/RSP — comparing field-by-field against a known-good AMD reference such as SimpleSvm.

### 8.4 Driver Verifier — use with caution on a hypervisor

Driver Verifier is built for **ordinary** drivers. Several of its checks assume normal driver context and can **induce faults from the host/exit context** (GIF cleared, undefined IRQL, private stack) that do not reflect a real bug — turning a benign path into a bugcheck or a wedge. Its value here is low anyway (this driver does almost no I/O and no pageable allocations). If you use it at all: enable only **targeted, low-risk** checks (e.g. Pool Tracking for the allocation path), **never** the full "Standard" set, **never** on exit-handler paths, and always on a snapshot with KD attached.

```bat
verifier /standard /driver simplevisor.sys   :: (reboot) — heavier than you usually want here
verifier /querysettings
verifier /reset                              :: (reboot) turn OFF
```

Because the service is demand-start, Verifier cannot load (and therefore cannot bugcheck) the driver at boot, so this will not create a boot loop here — but still only do it on a snapshotted VM with KD attached. In KD during a bugcheck, `!verifier` shows Verifier state and stacks.

### 8.5 The unload path is as delicate as load

`sc stop` exercises the GIF/segment-restore path on **every** CPU (via the ring-0 magic CPUID channel). The canonical bug in a hypervisor like this is **failing to restore the Global Interrupt Flag (STGI) before disabling SVM** — which leaves interrupts globally blocked forever and hangs the CPU after unload. In this project that path is **already fixed** (commit `fa8550d` restores GIF via STGI and reloads guest segment state in the correct order), so treat it as the textbook example of *why order matters*: reintroducing the wrong order reproduces a hang-on-stop. **Treat a `sc stop` as exactly as risky as a `sc start`** — snapshot first.

---

## 9. First milestones, in order — a checklist

Prove these one at a time, on a **snapshotted VM**, with the debugger attached and (for M1+) driver symbols loaded per [§5.6](#56-load-the-drivers-symbols-only-after-it-is-loaded). Don't move to the next until the current one holds.

- [ ] **M-1 — It compiles.** `Build: 1 succeeded` and `simplevisor.sys` + `.pdb` exist ([§0.4](#04-find-the-output-and-confirm-the-build-worked)). On untested code this is a real milestone, not a given.
- [ ] **M0 — Environment sane.** Nested AMD-V exposed to the VM (§2.3); **no other hypervisor owns SVM** (§3). Quick checks: `bcdedit /enum {current}` shows `hypervisorlaunchtype Off`; `msinfo32` does **not** say "A hypervisor has been detected."
- [ ] **M1 — Driver loads far enough to run code.** `bp simplevisor!DriverEntry` is hit after `sc start` (proves test-signing + service setup are correct — §4). This is the point at which you load driver symbols ([§5.6](#56-load-the-drivers-symbols-only-after-it-is-loaded)).
- [ ] **M2 — Detection passes.** Confirm SVM is usable: `bp simplevisor!ShvSvmProbe`, `g` to hit it, then `gu` (step out to the return) and `r al` — a **non-zero `AL`** means SVM present and not firmware-disabled (returned TRUE).
- [ ] **M3 — Round-trip succeeds.** `sc start` returns RUNNING **and** **`The SHV has been installed.`** prints. This one line proves a lot: SVM enabled, VMCB accepted, first VMRUN executed, guest resumed, CPUID interception live, and **every** CPU succeeded. If instead `sc start` fails, decode the status per [§6.4](#64-good-load-vs-failed-load-and-decoding-the-status).
- [ ] **M4 — Live detection from user mode.** Build and run the CPUID probe (§7.4b); expect `hypervisor-present bit == 1` **and** `SimpleVisor signature == 1`.
- [ ] **M5 — Transparency/stability.** Let the guest run normally (I/O, GUI, network) for a while with no crashes — proof the nested-page identity map is transparent.
- [ ] **M6 — Clean unload.** `sc stop simplevisor` → **`The SHV has been uninstalled.`** and the system stays stable; re-run the CPUID probe and confirm the `Shv ` signature is gone.
- [ ] **M7 (advanced, optional) — Post-mortem instrumentation.** Add the global VP-data anchor + ring buffer (§7.4c) and inspect `GuestVmcb` / stashed values with `dt` / `dq`. **This means editing the driver — follow the safety rules in [§7.6](#76-safety-when-adding-your-own-instrumentation) (mapped-memory writes only, one change at a time, re-sign + re-snapshot).**

---

## 10. Symptom → cause quick-reference table

| Symptom | Most likely cause | Where to look |
|---|---|---|
| **`shv.sln` won't open / no `NT|x64` config / toolset missing** | WDK or its Visual Studio extension not installed | §0.2 |
| **Build fails to compile** | Untested reference code | §0.5 (compare against SimpleSvm) |
| `sc start` → **"Access is denied" (5)** | Shell not elevated, or bare `sc` in PowerShell | §4.6 (use elevated `cmd.exe`, or `sc.exe`) |
| `sc start` → **"cannot find the file" (2)** | Bad/relative/unquoted `binPath=`, wrong file name, or `.sys` not copied to target | §1 (build name), §4.0 (copy), §4.6 |
| `sc start` → **577 "cannot verify digital signature"** | Not test-signed, test signing not active, **Secure Boot still on**, or cert not trusted on this machine | §4.2–§4.5 (`certutil -store Root` check in §4.5) |
| `sc start` → **1275 "driver has been blocked"** | HVCI / Memory Integrity on, or vulnerable-driver blocklist | §3, §4.9 |
| **`sc start` FAILS *and* driver prints `… (0xFFFFFFFF) …`** | No SVM / firmware-disabled / **another hypervisor owns SVM** (`DriverEntry` returned failure) | §3, §6.4, §8.3 |
| **`sc start` FAILS *and* driver prints `… (0xFFFFFFFE) …`** | Per-CPU memory allocation failed | §6.4 |
| **`sc start` FAILS *and* driver prints `… (0xFFFFFFFD) …`** | `VMRUN` ran but first exit was `VMEXIT_INVALID` — consistency checks rejected the VMCB; guest never ran | §6.4, §8.3 case B |
| **`sc start` fails but you see no driver line** | You have no kernel-output viewer attached (or DebugView lost it on a crash) — *not* a masking issue: ERROR lines show by default in a KD | §6.1, attach a KD (§5) then decode the `sc` code with `certutil -error` (§4.9) |
| **A `The SHV failed to initialize …` line with NO `sc` error** | Failed re-hyperjack **on resume from sleep** (power callback ignores the result) | §6.4 note, §7.5 (don't sleep the target) |
| **Instant reboot at `sc start`, no dump** | Triple fault: guest state that passed `VMRUN`'s checks but is unrunnable (bad RIP/segment base, NPT map, or host/GIF ordering) — *not* a grossly-bad VMCB, which reports 0xFFFFFFFD instead | §8.3, §7.4d (inspect VMCB before launch) |
| **Hard hang at `sc start`** | Bad nested-page map, or interrupt/GIF issue leaving the scheduler stalled | §8.3 (`~*k`, `!running`), revert + bisect |
| **Hang/freeze at `sc stop`** | Delicate unload path (GIF/STGI/segment restore) — already fixed here; a regression reintroduces it | §8.5 |
| **Bugcheck naming simplevisor.sys** | A fault caught in guest/normal context | `!analyze -v`, §8.2 |
| **Breakpoint in the exit handler never fires (or kills the box)** | Exit handler runs in **host** context (GIF cleared, own stack) — the debugger can't reach it | §7.1–§7.2 (never do this) |
| **Delayed crash/hang minutes after a *failed* load** | Partial load: some CPUs stayed hyperjacked after the image was freed | §7.5 (reboot/revert after any failed load) |
| **`0x109 CRITICAL_STRUCTURE_CORRUPTION` later** | PatchGuard (Kernel Patch Protection) — GDTR/IDTR limits left wrong on unload | §8.2, compare against reference |
| **Net debug "won't connect"** | Wrong `hostip` (must be the HOST), unsupported NIC, or HOST firewall blocking UDP | §5.2 (NIC check), §5.4 (firewall) |
| **`.reload /f simplevisor.sys` says "no symbols"/deferred** | Run before the driver was loaded, or `.pdb` doesn't match the loaded `.sys` | §5.6 (load symbols only after M1; recopy `.pdb` after every rebuild) |
| **Target frozen after closing WinDbg** | You detached while broken in — you must `g` first | §5.4 |
| **DebugView shows nothing while KD attached** | Kernel output is routed to the debugger; the two are mutually exclusive | §6.5 |