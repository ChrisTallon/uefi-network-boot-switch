/*

UEFI Network Boot Switch
Copyright (C) 2018 Chris Tallon

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
version 2, as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

*/

#include <efi.h>
#include <efilib.h>

static const EFI_GUID GlobalVariableGUID = EFI_GLOBAL_VARIABLE;
static const EFI_GUID SimpleNetworkGUID = EFI_SIMPLE_NETWORK_PROTOCOL;
static const EFI_GUID SimpleFileSystemGUID = SIMPLE_FILE_SYSTEM_PROTOCOL;
static const EFI_GUID LoadedImageGUID = LOADED_IMAGE_PROTOCOL;

EFI_SIMPLE_NETWORK* getNetwork();
EFI_STATUS transmitRequestPacket(EFI_SIMPLE_NETWORK* net_if_struct);
EFI_STATUS receivePacket(EFI_SIMPLE_NETWORK* net_if_struct, UINT16* rxBuffer);
char compareMacs(EFI_MAC_ADDRESS m1, EFI_MAC_ADDRESS m2);
EFI_STATUS getBootEntry(WCHAR* name, UINTN* entrySize, unsigned char** entryData);
EFI_DEVICE_PATH* getEntryDevicePath(UINTN size, unsigned char* data);
EFI_DEVICE_PATH* completeDevicePath(EFI_DEVICE_PATH* secondHalfDevicePath);
void sleep(UINTN tenths);
void d(EFI_STATUS status, const WCHAR* tag);
char loadServerMAC(EFI_HANDLE ImageHandle);

static const UINT16 ETHERNET_PROTOCOL = 0x88B6;
EFI_MAC_ADDRESS serverMAC;

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  InitializeLib(ImageHandle, SystemTable);
  Print(L"\n\nUEFI Network Boot Switch\n");

  if (loadServerMAC(ImageHandle))
  {
    Print(L"Server MAC: %x:%x:%x:%x:%x:%x\n",
        serverMAC.Addr[0],
        serverMAC.Addr[1],
        serverMAC.Addr[2],
        serverMAC.Addr[3],
        serverMAC.Addr[4],
        serverMAC.Addr[5]);
  }
  else
  {
    Print(L"Could not read server MAC address from file server.mac\n");
    Print(L"Returning to UEFI loader...\n");
    sleep(100);
    return EFI_SUCCESS;
  }

  EFI_SIMPLE_NETWORK* net_if_struct = getNetwork();
  if (!net_if_struct)
  {
    Print(L"Main: No network, returning to UEFI loader\n");
    return EFI_SUCCESS;
  }

  EFI_STATUS status;
  UINTN doNetworkStop = 0;

  if (net_if_struct->Mode->State == EfiSimpleNetworkStopped)
  {
    status = uefi_call_wrapper(net_if_struct->Start, 1,
                                net_if_struct);
    d(status, L"Main: net start");
    if (status != EFI_SUCCESS)
    {
      Print(L"Main: Could not start network, returning to UEFI loader\n");
      return EFI_SUCCESS; // FIXME or something else?
    }

    doNetworkStop = 1;
  }

  UINTN doNetworkShutdown = 0;

  if (net_if_struct->Mode->State == EfiSimpleNetworkStarted)
  {
    status = uefi_call_wrapper(net_if_struct->Initialize, 3,
                              net_if_struct, 0, 0);
    d(status, L"Main: net init");
    if (status != EFI_SUCCESS)
    {
      Print(L"Main: Could not initialise network, returning to UEFI loader\n");

      if (doNetworkStop)
      {
        status = uefi_call_wrapper(net_if_struct->Stop, 1,
                                  net_if_struct);
        d(status, L"Main: net stop");
      }

      return EFI_SUCCESS; // FIXME or something else?
    }

    doNetworkShutdown = 1;
  }


  UINT16 rxBuffer = 0xFFFF;
  UINTN txCount = 0;
  while(1)
  {
    Print(L"Transmit...\n");
    txCount++;
    transmitRequestPacket(net_if_struct);

    UINTN rxCount = 0; // FIXME Use WaitForPacket when we know how it works
    while(1)
    {
      status = receivePacket(net_if_struct, &rxBuffer);
      if (status == EFI_SUCCESS)
      {
        Print(L"Receive success on iteration: %d\n", rxCount);
        break;
      }
      if (++rxCount == 1000) break;
      if (status == EFI_NOT_READY) continue;
      d(status, L"Main: receivePacket");
    }

    if (status == EFI_SUCCESS) break;
    if (txCount == 3) break;
  }

  if (doNetworkShutdown)
  {
    status = uefi_call_wrapper(net_if_struct->Shutdown, 1,
                              net_if_struct);
    d(status, L"Main: net shutdown");
  }

  if (doNetworkStop)
  {
    status = uefi_call_wrapper(net_if_struct->Stop, 1,
                              net_if_struct);
    d(status, L"Main: net stop");
  }


  if (rxBuffer == 0xFFFF)
  {
    Print(L"Main: Net request failed, returning to UEFI loader\n");
    sleep(80);
    return EFI_SUCCESS; // FIXME or something else?
  }

  Print(L"Main: Data received: %4.0x\n", rxBuffer);
  WCHAR bootName[9];
  UINTN bootOptionSize = 0;
  unsigned char* bootOptionData = NULL;
  SPrint(bootName, 18, L"Boot%4.0x\n", rxBuffer);
  status = getBootEntry(bootName, &bootOptionSize, &bootOptionData);
  if (status != EFI_SUCCESS)
  {
    Print(L"Error 2: %r\n", status);
    Print(L"Main: Could not load boot entry, returning to UEFI loader\n");
    sleep(80);
    return EFI_SUCCESS; // FIXME or something else?
  }

  EFI_DEVICE_PATH* nextBootManagerDevPathHalf = getEntryDevicePath(bootOptionSize, bootOptionData);
  FreePool(bootOptionData);

  EFI_DEVICE_PATH* nextBootManagerDevPath = completeDevicePath(nextBootManagerDevPathHalf);
  FreePool(nextBootManagerDevPathHalf);

  Print(L"Final booting: %s\n", DevicePathToStr(nextBootManagerDevPath));

  EFI_HANDLE childImage;
  status = uefi_call_wrapper(SystemTable->BootServices->LoadImage, 6,
                             TRUE, ImageHandle, nextBootManagerDevPath, NULL, 0, &childImage);

  Print(L"Main: LoadImage: %r\n", status);

  FreePool(nextBootManagerDevPath);

  sleep(20);

  status = uefi_call_wrapper(SystemTable->BootServices->StartImage, 3,
                             childImage, NULL, NULL);
  Print(L"Main: StartImage: %r\n", status);

  // FIXME What to clean up if we are returned to?
  return EFI_SUCCESS;
}

// -----------------------------------------------------------------------------------------------

EFI_SIMPLE_NETWORK* getNetwork()
{
  EFI_SIMPLE_NETWORK* toReturn = NULL;

  UINTN numHandles = 0;
  EFI_HANDLE* handles = NULL;
  EFI_STATUS status = uefi_call_wrapper(BS->LocateHandleBuffer, 5,
                                        ByProtocol, &SimpleNetworkGUID, NULL, &numHandles, &handles);
  d(status, L"getNetwork: LocateHandleBuffer");

  Print(L"getNetwork: LocateHandleBuffer OK (%d handles)\n", numHandles);

  EFI_HANDLE handle = NULL;
  for (UINTN index = 0; index < numHandles; index++)
  {
    // FIXME For now return the first one. Somehow need to choose the right net device
    handle = handles[index];
    break;
  }
  FreePool(handles);

  if (!handle)
  {
    Print(L"getNetwork: No network handle\n");
    return NULL;
  }

  status = uefi_call_wrapper(BS->HandleProtocol, 3,
                             handle, &SimpleNetworkGUID, &toReturn);
  d(status, L"getNetwork: HandleProtocol");

  return toReturn;
}

EFI_STATUS transmitRequestPacket(EFI_SIMPLE_NETWORK* net_if_struct)
{
  void* packet;
  UINTN headerSize = net_if_struct->Mode->MediaHeaderSize;

  EFI_STATUS status = uefi_call_wrapper(BS->AllocatePool, 3,
                             EfiLoaderData, headerSize + 4, &packet);
  d(status, L"Transmit: mem alloc");

  unsigned char* packetc = (unsigned char*)packet;
  packetc[headerSize] = 0xB0;
  packetc[headerSize+1] = 0x07;
  packetc[headerSize+2] = 0xB0;
  packetc[headerSize+3] = 0x07;

  status = uefi_call_wrapper(net_if_struct->Transmit, 7,
                             net_if_struct, headerSize, headerSize + 4, packet, NULL, &serverMAC, &ETHERNET_PROTOCOL);
  d(status, L"Transmit: Transmit");




  //////////////////////////////////////////////////////////////////
  /*
   * Major FIXME. Somehow, having this receive attempt here (which fails,
   * presumably it's too early) allows the receive loop in efi_main
   * to work after several iterations of the loop. ???
   * Are there serious timing issues? Is this delaying starting the main
   * receive loop to just the right time?
   *
   * Would prefer to avoid all this and have separate transmit and
   * receive functions by using the WaitForPacket event in simple
   * networking, but it doesn't seem to work.
   * */

  UINTN receivedHeaderSize;
  UINTN receivedBufferSize = 1024;
  unsigned char* receivedBuffer = AllocateZeroPool(receivedBufferSize);
  EFI_MAC_ADDRESS receivedSrcAddress;
  UINT16 receivedProtocol;

  status = uefi_call_wrapper(net_if_struct->Receive, 7,
                             net_if_struct, &receivedHeaderSize, &receivedBufferSize, receivedBuffer,
                             &receivedSrcAddress, NULL, &receivedProtocol);

  Print(L"ReceivePacket: 1 Receive status: %r\n", status);
/*
  Print(L"ReceivePacket: 1 Received src MAC: %x:%x:%x:%x:%x:%x\n",
        receivedSrcAddress.Addr[0],
        receivedSrcAddress.Addr[1],
        receivedSrcAddress.Addr[2],
        receivedSrcAddress.Addr[3],
        receivedSrcAddress.Addr[4],
        receivedSrcAddress.Addr[5]);
*/
  FreePool(receivedBuffer);

  // FIXME - end of extra receive code
  //////////////////////////////////////////////////////////////////



  FreePool(packet);
  return status;
}

EFI_STATUS receivePacket(EFI_SIMPLE_NETWORK* net_if_struct, UINT16* rxBuffer)
{
  UINTN receivedHeaderSize;
  UINTN receivedBufferSize = 1024;
  unsigned char* receivedBuffer = AllocateZeroPool(receivedBufferSize);
  EFI_MAC_ADDRESS receivedSrcAddress;
  UINT16 receivedProtocol;

  EFI_STATUS status = uefi_call_wrapper(net_if_struct->Receive, 7,
                             net_if_struct, &receivedHeaderSize, &receivedBufferSize, receivedBuffer,
                             &receivedSrcAddress, NULL, &receivedProtocol);

//  Print(L"ReceivePacket: 2 Receive status: %r\n", status);

  if (status == EFI_SUCCESS)
  {
    Print(L"ReceivePacket: Bytes received: %d\n", receivedBufferSize);
    Print(L"ReceivePacket: Header size: %d\n", receivedHeaderSize);
    Print(L"ReceivePacket: ReceivedProtocol: %x\n", receivedProtocol);
    Print(L"ReceivePacket: Received src MAC: %x:%x:%x:%x:%x:%x\n",
          receivedSrcAddress.Addr[0],
          receivedSrcAddress.Addr[1],
          receivedSrcAddress.Addr[2],
          receivedSrcAddress.Addr[3],
          receivedSrcAddress.Addr[4],
          receivedSrcAddress.Addr[5]);

    if ((receivedBufferSize - receivedHeaderSize) < 5)
      Print(L"ReceivePacket: Packet too short\n");

    if (compareMacs(serverMAC, receivedSrcAddress)) // FIXME receivedSrcAddress is garbage
      Print(L"ReceivePacket: MAC compare OK\n");
    else
      Print(L"ReceivePacket: MAC compare FAIL\n");

    if (receivedProtocol == ETHERNET_PROTOCOL)
      Print(L"ReceivePacket: protocol OK\n");
    else
      Print(L"ReceivePacket: protocol FAIL\n");

    if (   (receivedBuffer[receivedHeaderSize    ] != 0xB0)
        || (receivedBuffer[receivedHeaderSize + 1] != 0x07)
        || (receivedBuffer[receivedHeaderSize + 2] != 0xB0)
        || (receivedBuffer[receivedHeaderSize + 3] != 0x07) )
      Print(L"ReceivePacket: Magic FAIL\n");
    else
      Print(L"ReceivePacket: Magic OK\n");

    // FIXME implement rejecting based on these checks

    UINT16* payload = (UINT16*)&receivedBuffer[receivedHeaderSize + 4];
    Print(L"ReceivePacket: Data received: %x\n", *payload);
    *rxBuffer = *payload;
  }

  FreePool(receivedBuffer);
  return status;
}

char compareMacs(EFI_MAC_ADDRESS m1, EFI_MAC_ADDRESS m2)
{
  for (int i = 0; i < 6; i++)
  {
    Print(L"compareMACS: %x %x\n", m1.Addr[i], m2.Addr[i]);
    if (m1.Addr[i] != m2.Addr[i]) return 0;
  }
  return 1;
}

EFI_STATUS getBootEntry(WCHAR* name, UINTN* entrySize, unsigned char** entryData) // Caller must Free entryData if EFI_SUCCESS
{
  UINTN bufferSize = 0;
  unsigned char* buffer = NULL;
  EFI_STATUS status;

  status = uefi_call_wrapper(RT->GetVariable, 5,
                             name, &GlobalVariableGUID, NULL, &bufferSize, buffer);
  if (status != EFI_BUFFER_TOO_SMALL) return EFI_NOT_FOUND;

  buffer = AllocateZeroPool(bufferSize);

  status = uefi_call_wrapper(RT->GetVariable, 5,
                             name, &GlobalVariableGUID, NULL, &bufferSize, buffer);
  if (status != EFI_SUCCESS)
  {
    FreePool(buffer);
    return EFI_NOT_FOUND;
  }

  *entrySize = bufferSize;
  *entryData = buffer;
  return EFI_SUCCESS;
}

EFI_DEVICE_PATH* getEntryDevicePath(UINTN size, unsigned char* data) // Caller must Free data
{
  UINTN pos = 4; // Skip 4 bytes of attributes

  UINT16 filePathListLength;
  CopyMem(&filePathListLength, &data[pos], 2);
  pos += 2;

  UINTN descriptionLengthW = StrLen((WCHAR*)&data[pos]);
  WCHAR* description = AllocateZeroPool((descriptionLengthW * 2) + 2);
  CopyMem(description, &data[pos], (descriptionLengthW * 2) + 2);
  pos += (descriptionLengthW * 2) + 2;
  Print(L"\nBOOTING: description: '%s'\n\n", description);
  FreePool(description);

  unsigned char* filePathList = AllocateZeroPool(filePathListLength);
  CopyMem(filePathList, &data[pos], filePathListLength);

  UINTN dpSize = DevicePathSize((EFI_DEVICE_PATH*)filePathList);

  EFI_DEVICE_PATH* toReturn = AllocateZeroPool(dpSize);
  CopyMem(toReturn, filePathList, dpSize); // Return only the first FilePathList[]
  FreePool(filePathList);

  Print(L"getEntryDevicePath: Booting: %s\n", DevicePathToStr(toReturn));
  return toReturn;
}

void sleep(UINTN tenths)
{
  EFI_EVENT event;
  EFI_STATUS status = uefi_call_wrapper(BS->CreateEvent, 5,
                                        EVT_TIMER, 0, NULL, NULL, &event);
  d(status, L"sleep: create event");

  status = uefi_call_wrapper(BS->SetTimer, 3,
                             event, TimerRelative, 1000000 * tenths);
  d(status, L"sleep: SetTimer");

  EFI_EVENT waitEvents[1];
  waitEvents[0] = event;
  UINTN eventTriggered;
  status = uefi_call_wrapper(BS->WaitForEvent, 3,
                             1, waitEvents, &eventTriggered);
  d(status, L"sleep: WaitForEvent");

  status = uefi_call_wrapper(BS->CloseEvent, 1,
                             event);
  d(status, L"sleep: CloseEvent");
}

void d(EFI_STATUS status, const WCHAR* tag)
{
  if (status != EFI_SUCCESS)
  {
    Print(L"%s - failed with status: %r\n", tag, status);
  }
}

EFI_DEVICE_PATH* completeDevicePath(EFI_DEVICE_PATH* secondHalf) // Caller must Free returned EFI_DEVICE_PATH
{
  EFI_DEVICE_PATH* toReturn = NULL;

  UINTN numHandles = 0;
  EFI_HANDLE* handles = NULL;
  EFI_STATUS status = uefi_call_wrapper(BS->LocateHandleBuffer, 5,
                             ByProtocol, &SimpleFileSystemGUID, NULL, &numHandles, &handles);

  if (status != EFI_SUCCESS)
  {
    d(status, L"completeDevicePath: LocateHandleBuffer");
    return NULL;
  }

  EFI_DEVICE_PATH* devPath;
  EFI_DEVICE_PATH* partPointer;
  for (UINTN index = 0; index < numHandles; index++)
  {
    devPath = DevicePathFromHandle(handles[index]);  // Fairly sure we don't FreePool this
    partPointer = devPath;
    // partPointer will now be the full EFI_DEVICE_PATH for one of the detected drives, e.g.
    // PciRoot(0)/...etc.../HD(Part1,Sig...etc...)

    // While the *next* node isn't the end, advance partPointer. Then, partPointer will start at HD.
    while(!IsDevicePathEnd(NextDevicePathNode(partPointer)))
    {
      partPointer = NextDevicePathNode(partPointer);
    }

    // secondHalf is a EFI_DEVICE_PATH starting at HD and ending with a EFI boot manager filename

    if(LibMatchDevicePaths(partPointer, secondHalf)) // Compares the start of secondHalf to devPath
    {
      // Match. This is the right drive. Advance secondHalf so it starts at the filesystem path
      secondHalf = NextDevicePathNode(secondHalf);
      toReturn = AppendDevicePath(devPath, secondHalf);
      break;
    }
  }

  FreePool(handles);
  return toReturn;
}

char loadServerMAC(EFI_HANDLE thisImage)
{
  EFI_LOADED_IMAGE* efiLI = NULL;
  EFI_STATUS status = uefi_call_wrapper(BS->HandleProtocol, 3,
                                 thisImage, &LoadedImageGUID, &efiLI);
  Print(L"loadServerMAC: HandleProtocol 1: %r\n", status);
  if (status != EFI_SUCCESS) return 0;

  EFI_HANDLE deviceHandle = efiLI->DeviceHandle;

  EFI_FILE_IO_INTERFACE* efiSF = NULL;
  status = uefi_call_wrapper(BS->HandleProtocol, 3,
                                 deviceHandle, &SimpleFileSystemGUID, &efiSF);
  Print(L"loadServerMAC: HandleProtocol 2: %r\n", status);
  if (status != EFI_SUCCESS) return 0;

  EFI_FILE* root;
  status = uefi_call_wrapper(efiSF->OpenVolume, 2,
                             efiSF, &root);
  Print(L"loadServerMAC: OpenVolume: %r\n", status);
  if (status != EFI_SUCCESS) return 0;

  EFI_FILE* macFile;
  status = uefi_call_wrapper(root->Open, 5,
                             root, &macFile, L"\\EFI\\UNBS\\server.mac", EFI_FILE_MODE_READ, 0);
  Print(L"loadServerMAC: Open file: %r\n", status);
  if (status != EFI_SUCCESS) return 0;

  UINTN dataSize = 17;
  uint8_t data[17];

  for(int j = 0; j < 17; j++) data[j] = 0;

  status = uefi_call_wrapper(macFile->Read, 3,
                             macFile, &dataSize, data);
  Print(L"loadServerMAC: Read file: %r\n", status);

  if (uefi_call_wrapper(macFile->Close, 1, macFile) != EFI_SUCCESS)
    Print(L"FAILED TO CLOSE server.mac !!\n");

  if (status != EFI_SUCCESS) return 0;

  uint8_t outpos = 0;
  uint8_t temp;

  // Convert ASCII MAC to binary. Is there anything like scanf in GNU-EFI?
  for(uint8_t i = 0; i < 17; i++)
  {
    if ((i == 2) || (i == 5) || (i == 8) || (i == 11) || (i == 14)) continue;

    if      ((data[i] > 47) && (data[i] <  58)) temp = data[i] - 48;
    else if ((data[i] > 64) && (data[i] <  71)) temp = data[i] - 55;
    else if ((data[i] > 96) && (data[i] < 103)) temp = data[i] - 87;

    if      ((i == 0) || (i == 3) || (i == 6) || (i == 9) || (i == 12) || (i == 15))
      serverMAC.Addr[outpos] = temp * 0x10;
    else if ((i == 1) || (i == 4) || (i == 7) || (i == 10) || (i == 13) || (i == 16))
      serverMAC.Addr[outpos++] += temp;
  }

  return 1;
}
