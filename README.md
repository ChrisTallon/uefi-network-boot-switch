# UEFI Network Boot Switch

Select which local O/S to boot by asking a network server.

## Why?

To automate booting the desired operating system on a PC when there is a choice, and have it remotely controllable.

## What is this?

It's an EFI application which runs at boot time before a boot manager. It sends a network request to a known MAC address and waits a few seconds for a reply. The reply contains a number - this is the UEFI Boot entry number in the EFI NVRAM. The corresponding boot manager is then started. If anything goes wrong control is handed back to the UEFI firmware which moves on to the next boot option.

## What is this not?

It is nothing to do with PXE or UEFI network booting where the whole operating system is loaded from the network.

## What can it boot?

Presumably any standard EFI boot manager / loader which is installed as a boot option in the NVRAM. So far it is known to boot Grub and Windows 10. (However, there is the possibility of parameters for bootloaders stored in NVRAM, as far as I can tell. UNBS does not pass these parameters (yet?). On my development machine Grub has no extra data and so can't be affected. Windows does, but boots fine without it.)

## Code Maturity

None! It's alpha quality at best. It's known to work on just one PC so far. There are a number of things yet to be worked on. Also, success will be largely dependent on the quality and completeness of the EFI firmware.

I suspect that because of the polling method used to wait for reply packets there are timing issues with catching received packets. This results in one out of every several boots not working correctly. I think this would be fixed by using WaitForPacket instead of polling but I haven't got this to work yet.

Comments and contributions welcome.

## To-Dos

* Work out better program exit codes to return to the UEFI loader
* Figure out what to do if the second boot manager returns
* Handle multiple network interfaces somehow
* Implement validity checking on returned packet

But the big ones:

* Use IP & DHCP & UDP instead of Ethernet (The EFI network stack is not as tall as I expected, or I haven't found it yet)
* Use WaitForPacket instead of loop-polling waiting for the return packet. At best, polling is just bad, at worst it might cause breaking timing problems

## Installation

### Build machine: Compilation

First, install dependencies (Ubuntu 18.04 e.g.):

    sudo apt-get install build-essential gnu-efi

Then, in the unbs directory:

    make

That should result in the unbs.efi file.

Now make a file called server.mac, edit with a text editor and at the beginning of the first line write the MAC address of the server, e.g. "00:11:22:33:44:55".

### Target machine

Obligatory warning: This process modifies the target machine's boot process. If you get something wrong (or I've got these instructions wrong) and your EFI boot process breaks you get to keep the pieces, and you get to repair it. Please be familiar with the EFI boot system - figuring out how to repair a dead EFI boot sequence is Not A Fun Job.

Another warning: Machines using Secure Boot won't work.

Copy your unbs.efi and server.mac to your EFI partition, in \EFI\UNBS\. On an Ubuntu machine this should be at /boot/efi/EFI/UNBS/ - since the EFI partition is automatically mounted to /boot/efi.

Now you need to create an EFI NVRAM entry for UNBS.

On Ubuntu there is a command - efibootmgr. An example command to create a new entry using the 2nd partition on the first hard disk as the EFI partition is as follows, edit to suit your system:

    sudo efibootmgr -c -p 2 -l "\EFI\UNBS\unbs.efi" -L UNBS

Now you want to set UNBS as the first boot option, followed by your normal preferred first boot option. Run efibootmgr with no arguments to see the current list of boot options. Let's imagine you have a dual boot machine between Linux and Windows, and your normal default boot option is Linux (booted by Grub). Let's imagine that efibootmgr reveals the boot option code for Grub is 0012, the code for Windows is 0006 and the newly created code for UNBS is 0014. Set the boot order as follows:

    sudo efibootmgr -o 0014,0012
    
Your machine's EFI firmware will attempt to boot UNBS first and will fall back to Grub if UNBS fails.

### Server machine

A small sample server is in the unbs-server directory. It reads a file (unbs-server.db) containing the client machine information and waits for request packets. It re-reads the client database file on receiving a SIGUSR1. Run 'make' in the server directory to compile the binary.

The database file contains three lines for each client machine. The first line is ignored - you can document which machine this entry is for on this line. (I also note the possible boot codes for the client here too). The second line is the MAC address of the client, and the third line is the four digit hex code for the required boot entry on the client.

As mentioned before this is a sample bare bones server which really is only to demonstrate how to reply to the clients. However, I currently use it as a systemd service and some scripts to switch out the config file and send the USR1 signal. 

## Feedback

Questions? Comments? Contributions? Please email me at chris@loggytronic.com.

If you actually use UNBS and find it useful please drop me an email - I'd be gratified to know someone got it working ;)
