/*

UEFI Network Boot Switch Server
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <signal.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>

#define MAGIC { 0xB0, 0x07, 0xB0, 0x07 }
#define ETHER_PROTOCOL 0x88B6
#define MAX_CLIENTS 10

typedef struct clients_tt
{
  uint8_t address[6];
  uint16_t bytes;
} clients_t;

clients_t clients[MAX_CLIENTS];
uint8_t numClients;

void sendPacket(int fd, int ifindex, uint8_t* to, uint8_t* buffer, ssize_t length);
void handleSignal(int sigNum);
int readDB();

char reReadDB = 0;

int main()
{
  const uint8_t magicBytes[4] = MAGIC;

  pid_t myPid = getpid();
  FILE* pidFile = fopen("unbs-server.pid", "w");
  fprintf(pidFile, "%d", myPid);
  fclose(pidFile);

  struct sigaction sig = {
    .sa_handler = handleSignal,
    .sa_flags = 0,
  };
  sigaction(SIGUSR1, &sig, NULL);

  if (!readDB()) exit(-1);

  int fd = socket(AF_PACKET, SOCK_DGRAM, htons(ETHER_PROTOCOL));
  if (fd < 0)
  {
    perror("SOCKET");
    exit(-1);
  }
 
  // Make loop variables

  const int bufferSize = 100;
  uint8_t buffer[bufferSize];
  ssize_t got = 0;
  struct sockaddr_ll srcAddr;
  socklen_t srcAddrLen;

  // Loop waiting for queries

  while(1)
  {
    srcAddrLen = sizeof(struct sockaddr_ll);
    got = recvfrom(fd, buffer, bufferSize, 0, (struct sockaddr *)&srcAddr, &srcAddrLen);
    if (reReadDB)
    {
      readDB();
      reReadDB = 0;
      continue;
    }

    if (ntohs(srcAddr.sll_protocol) != ETHER_PROTOCOL)
    {
      printf("Error: Received %i byte packet but with wrong ether protocol!\n", got);
      continue;
    }

    if (srcAddr.sll_halen != 6)
    {
      printf("Error: Address length isn't 6.\n");
      continue;
    }

    printf("Received %i byte packet on interface %d from %x:%x:%x:%x:%x:%x\n",
           got, srcAddr.sll_ifindex, srcAddr.sll_addr[0], srcAddr.sll_addr[1],
           srcAddr.sll_addr[2], srcAddr.sll_addr[3], srcAddr.sll_addr[4], srcAddr.sll_addr[5]);

    if (got < 4)
    {
      printf("Packet too short\n");
      continue;
    }

    if (memcmp(magicBytes, buffer, 4))
    {
      printf("Magic bytes incorrect\n");
      continue;
    }

    memset(buffer, 0, bufferSize);
    memcpy(buffer, magicBytes, 4);

    int i;
    for(i = 0; i < numClients; i++)
    {
      printf("%x %x %x %x %x %x\n", 
        clients[i].address[0],
        clients[i].address[1],
        clients[i].address[2],
        clients[i].address[3],
        clients[i].address[4],
        clients[i].address[5]
      );
    
    
      if (!memcmp(clients[i].address, srcAddr.sll_addr, 6))
      {
        // Found client
        unsigned short* target = (unsigned short*)&buffer[4];
        *target = clients[i].bytes;
        sendPacket(fd, srcAddr.sll_ifindex, srcAddr.sll_addr, buffer, 6);
        break;
      }
    }

    if (i == numClients) // Not found - reply with fail code
    {
      unsigned short* target = (unsigned short*)&buffer[4];
      *target = 0xFFFF;
      sendPacket(fd, srcAddr.sll_ifindex, srcAddr.sll_addr, buffer, 6);
    }
  }


  return 0;
}

void sendPacket(int fd, int ifindex, uint8_t* to, uint8_t* buffer, ssize_t length)
{
  struct sockaddr_ll destAddr;
  memset(&destAddr, 0, sizeof(struct sockaddr_ll));
  destAddr.sll_family = AF_PACKET;
  destAddr.sll_protocol = htons(ETHER_PROTOCOL);
  destAddr.sll_ifindex = ifindex;
  destAddr.sll_halen = ETH_ALEN;
  memcpy(destAddr.sll_addr, to, 6);
  ssize_t sent = sendto(fd, buffer, length, 0, (struct sockaddr*)&destAddr, sizeof(struct sockaddr_ll));
  if (sent != length)
  {
    perror("SEND");
  }
}

void handleSignal(int sigNum)
{
  reReadDB = 1;
}

int readDB()
{
  char buffer[1024];
  uint8_t mac[6];
  uint16_t bytes;
  FILE* dbFile = fopen("unbs-server.db", "r");
  if (!dbFile) return 0;
  int r;
  numClients = 0;
  for (int i = 0; i < MAX_CLIENTS; i++)
  {
    if (feof(dbFile)) break;
    if (!fgets(buffer, 1024, dbFile)) break;
    if (feof(dbFile)) break;
    r = fscanf(dbFile, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]);
    if (r != 6) break;
    if (feof(dbFile)) break;
    r = fscanf(dbFile, "%hx", &bytes);
    if (r != 1) break;
    if (feof(dbFile)) break;
    clients[i].bytes = bytes;
    clients[i].address[0] = mac[0];
    clients[i].address[1] = mac[1];
    clients[i].address[2] = mac[2];
    clients[i].address[3] = mac[3];
    clients[i].address[4] = mac[4];
    clients[i].address[5] = mac[5];
    numClients++;
  }
  
  fclose(dbFile);
  if (numClients)
    printf("Read DB ok - %d clients\n", numClients);
  else
    printf("0 clients read from DB... Problem...\n");
    
  return 1;
}
