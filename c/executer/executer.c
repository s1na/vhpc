#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>

#include <czmq.h>
#include "VBoxCAPIGlue.h"

#include "executer.h"
#include "utils.h"
#include "oe.pb-c.h"
#include "ee.pb-c.h"

#define TP_PASS "123"
#define TP_PORT 23632
#define TP_SOURCE "172.17.10.80"
#define TP_TARGET "172.17.8.165"



struct VMMetadata {
  char *home;
  int n_history;
  char **history;
};

IVirtualBoxClient *vboxclient = NULL;
IVirtualBox *vbox = NULL;
ISession *session = NULL;
ISession *tp_session = NULL;

struct VMMetadata *metadatas;
int metadatas_len;

char *host_ip;

zactor_t *comm;

int main(int argc, char *argv[]) {
  printf("Executer started.\n");
  zsock_t *organizer_sock = zsock_new_rep("tcp://127.0.0.1:98789");

  ULONG revision = 0;
  HRESULT rc;

  // Initialize objects
  if (VBoxCGlueInit()) {
    err_exit(g_szVBoxErrMsg);
  }

  g_pVBoxFuncs->pfnClientInitialize(NULL, &vboxclient);
  if (!vboxclient) {
    err_exit("Failed to initialize client.");
  }
  rc = IVirtualBoxClient_get_VirtualBox(vboxclient, &vbox);
  if (FAILED(rc) || !vbox) {
    err_exit("Could not get VirtualBox reference");
  }
  rc = IVirtualBoxClient_get_Session(vboxclient, &session);
  if (FAILED(rc) || !session) {
    err_exit("Could not get Session reference");
  }

  metadatas_len = 0;
  metadatas = malloc(5 * sizeof(struct VMMetadata));

  host_ip = malloc(20 * sizeof(char));
  host_ip = get_host_ip(host_ip);

  comm = zactor_new(comm_actor, NULL);

  char *smsg;
  while (1) {
    smsg = zstr_recv(organizer_sock);
    if (smsg == NULL) {
      break;
    }

    OEMsg *oem;
    /*Teleport *tpm;*/
    int len = strlen(smsg);
    if (!len) {
      log_err("Message rcvd from organizer empty.\n");
    }

    oem = oemsg__unpack(NULL, len, smsg);
    if (oem->type == OEMSG__TYPE__TELEPORT) {
      /*Teleport *tpm = oem->teleport;*/
      /*printf("[Teleport] VMName: %s\tTarget: %s\n", tpm->vm_name, tpm->target_ip);*/
      printf("Teleport msg recvd.\n");
      zstr_send(organizer_sock, "ok");
      zstr_send(comm, smsg);
    }

    zstr_free(&smsg);
  }

  zsock_destroy(&organizer_sock);
  free(host_ip);

  /*sleep(2);*/
  /*zstr_send(comm, "1");*/

  _exit(0);
}

void _find_machine(char *vmname, IMachine **machine) {
  HRESULT rc;
  BSTR vmname_utf16;
  g_pVBoxFuncs->pfnUtf8ToUtf16(vmname, &vmname_utf16);
  rc = IVirtualBox_FindMachine(vbox, vmname_utf16, machine);
  if (FAILED(rc) | !machine) {
    err_exit("Could not find machine.");
  }
  g_pVBoxFuncs->pfnUtf16Free(vmname_utf16);
}

void _start_vm(IMachine *machine, ISession *sess) {
    IProgress *progress;
    BSTR type, env;
    HRESULT rc;
    g_pVBoxFuncs->pfnUtf8ToUtf16("gui", &type);
    rc = IMachine_LaunchVMProcess(machine, sess, type, env, &progress);
    if (FAILED(rc)) {
      err_exit("Failed to start vm.");
    }
    g_pVBoxFuncs->pfnUtf16Free(type);

    IProgress_WaitForCompletion(progress, -1);
}

void comm_actor(zsock_t *pipe, void *args) {
  zsock_signal(pipe, 0);

  HRESULT rc;
  zsock_t *listener = zsock_new_rep("tcp://0.0.0.0:23432");
  zsock_t *sender;
  zpoller_t *poller = zpoller_new(pipe, listener, NULL);
  if (!poller) err_exit("Poller was not created.\n");

  char *target_ip = malloc(20 * sizeof(char));

  bool terminated = false;
  while (!terminated) {
    zsock_t *which = zpoller_wait(poller, -1);
    if (!zpoller_terminated(poller)) {
      if (which == listener) {
        char *smsg = zstr_recv(listener);
        printf("SMSG %s\n", smsg);
        // Target teleport init
        if (*smsg == '1') {
          zsock_signal(listener, 0);
          char *remote_ip = zstr_recv(listener);
          printf("Remote Addr: %s\n", remote_ip);
          zstr_free(&remote_ip);

          IMachine *tp_machine;
          _find_machine("fdr", &tp_machine);
          rc = IVirtualBoxClient_get_Session(vboxclient, &tp_session);
          if (FAILED(rc) || !tp_session) {
            err_exit("Could not get Session reference for teleport.");
          }
          IMachine_LockMachine(tp_machine, tp_session, LockType_Write);
          ISession_get_Machine(tp_session, &tp_machine);
          IMachine_SetTeleporterEnabled(tp_machine, true);
          BSTR hostnameu, passu;
          g_pVBoxFuncs->pfnUtf8ToUtf16(host_ip, &hostnameu);
          g_pVBoxFuncs->pfnUtf8ToUtf16(TP_PASS, &passu);
          IMachine_SetTeleporterAddress(tp_machine, hostnameu);
          IMachine_SetTeleporterPassword(tp_machine, passu);
          g_pVBoxFuncs->pfnUtf16Free(hostnameu);
          IMachine_SetTeleporterPort(tp_machine, TP_PORT);
          /*IConsole_SetUseHostClipboard(true);*/
          IMachine_SaveSettings(tp_machine);
          ISession_UnlockMachine(tp_session);

          ISession_Release(tp_session);
          tp_session = NULL;
          IMachine_Release(tp_machine);
          tp_machine = NULL;

          rc = IVirtualBoxClient_get_Session(vboxclient, &tp_session);
          if (FAILED(rc) || !tp_session) {
            err_exit("Could not get Session reference for teleport.");
          }
          _find_machine("fdr", &tp_machine);
          sleep(1);
          /*_start_vm(tp_machine, tp_session);*/

          IProgress *tp_progress;
          BSTR type, env;
          g_pVBoxFuncs->pfnUtf8ToUtf16("gui", &type);
          rc = IMachine_LaunchVMProcess(tp_machine, tp_session, type, env, &tp_progress);
          if (FAILED(rc)) {
            err_exit("Failed to start vm.");
          }
          g_pVBoxFuncs->pfnUtf16Free(type);

          sleep(3);

          zstr_send(listener, "2");

          IProgress_WaitForCompletion(tp_progress, -1);
          ISession_UnlockMachine(tp_session);

          char *smd = zstr_recv(listener);
          TeleportMetadata *md;
          int len = strlen(smd);
          printf(smd);
          md = teleport_metadata__unpack(NULL, len, smd);
          printf("VM Metadata arrived.\n");
          printf("Home: %s\t", md->home);
          zstr_free(&smd);

          struct VMMetadata vm_md = {home: md->home, n_history: md->n_history + 1, history: md->history};
          vm_md.history = realloc(vm_md.history, (vm_md.n_history) * sizeof(char*));
          vm_md.history[vm_md.n_history - 1] = host_ip;
          if (metadatas_len % 5 == 0) {
            metadatas = realloc(metadatas, (metadatas_len + 5) * sizeof(struct VMMetadata));
          }
          metadatas[len++] = vm_md;

          printf("VM MD object home: %s\thistory_len: %d\n", vm_md.home, vm_md.n_history);
          int i = 0;
          for (i = 0; i <= md->n_history; i++) {
            printf("%s\n", md->history[i]);
          }
        }
        zstr_free(&smsg);
      } else if (sender && (which == sender)) {
        char *smsg = zstr_recv(sender);
        if (*smsg == '2') {
          // Source do teleport
          IMachine *tp_machine;
          rc = IVirtualBoxClient_get_Session(vboxclient, &tp_session);
          if (FAILED(rc) || !tp_session) {
            err_exit("Could not get session for teleport.\n");
          }
          _find_machine("fdr", &tp_machine);
          _start_vm(tp_machine, tp_session);

          IConsole *tp_console;
          rc = ISession_get_Console(tp_session, &tp_console);
          if (FAILED(rc) || !tp_console) {
            log_err("Couldnt get console for session in teleport.\n");
          }

          IProgress *progress;
          BSTR hostnameu, passu;
          g_pVBoxFuncs->pfnUtf8ToUtf16(target_ip, &hostnameu);
          g_pVBoxFuncs->pfnUtf8ToUtf16(TP_PASS, &passu);
          rc = IConsole_Teleport(tp_console, hostnameu, TP_PORT, passu, 250, &progress);
          if (FAILED(rc)) {
            log_err("Teleport Failed. ");
            printf("Retrun code %x\n", rc);
          }
          g_pVBoxFuncs->pfnUtf16Free(hostnameu);
          g_pVBoxFuncs->pfnUtf16Free(passu);

          IProgress_WaitForCompletion(progress, -1);

          TeleportMetadata md = TELEPORT_METADATA__INIT;
          void *buf;
          unsigned len;
          md.home = host_ip;
          md.n_history = 1;
          md.history = malloc(md.n_history * sizeof(char*));
          md.history[0] = host_ip;
          len = teleport_metadata__get_packed_size(&md);
          buf = malloc(len);
          teleport_metadata__pack(&md, buf);

          zstr_send(sender, buf);
          free(buf);
        }
        zstr_free(&smsg);
      } else {
        zmsg_t *msg = zmsg_recv(pipe);
        if (!msg)
            break; //  Interrupted
        char *smsg = zmsg_popstr(msg);
        //  All actors must handle $TERM in this way
        if (streq(smsg, "$TERM"))
            terminated = true;

        OEMsg *oem;
        int len = strlen(smsg);
        if (!len) {
          log_err("Message rcvd from executer empty.\n");
        }

        oem = oemsg__unpack(NULL, len, smsg);
        if (oem->type == OEMSG__TYPE__TELEPORT) {
          printf("Teleport.\n", smsg);
          char addr[50] = {0};
          sprintf(addr, "tcp://%s:%d", oem->teleport->target_ip, 23432);
          target_ip = oem->teleport->target_ip;
          printf("%s\n", addr);
          sender = zsock_new_req(addr);
          zstr_send(sender, "1");
          zsock_wait(sender);
          zstr_send(sender, host_ip);
          zpoller_add(poller, sender);
          /*zstr_send(pipe, "ok");*/
        }

        free(smsg);
        zmsg_destroy(&msg);
      }
    }
  }

  free(target_ip);

  zpoller_destroy(&poller);
  zsock_destroy(&listener);
  printf("%s\n", "Actor finished.");
}

void start_vm(char *name) {
  // Find machine
  IMachine *machine;
  _find_machine(name, &machine);
  _start_vm(machine, session);
}

void teleport(char *name, char *target) {
  char msg[50];
  sprintf(msg, "1%s", target);
  zstr_send(comm, msg);
  char *st = zstr_recv(comm);
  printf("%s\n", st);
  free(st);
}

char *get_host_ip(char *host) {
  struct ifaddrs *ifaddr, *ifa;
  int family, s;

  if (getifaddrs(&ifaddr) == -1) 
  {
    perror("getifaddrs");
    exit(EXIT_FAILURE);
  }

  for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) 
  {
    if (ifa->ifa_addr == NULL)
      continue;

    s=getnameinfo(ifa->ifa_addr,sizeof(struct sockaddr_in),host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);

    if((strcmp(ifa->ifa_name,"eth0")==0 || strcmp(ifa->ifa_name,"p4p1")==0)&&(ifa->ifa_addr->sa_family==AF_INET))
    {
      if (s != 0)
      {
          printf("getnameinfo() failed: %s\n", gai_strerror(s));
          exit(EXIT_FAILURE);
      }
      return host;
    }
  }

  freeifaddrs(ifaddr);
}

void log_err(char *err) {
  fprintf(stderr, err);
}

void _exit(int status) {
  if (session) {
    ISession_Release(session);
    session = NULL;
  }
  if (vbox) {
    IVirtualBox_Release(vbox);
    vbox = NULL;
  }
  if (vboxclient) {
    IVirtualBoxClient_Release(vboxclient);
    vboxclient = NULL;
  }
  zactor_destroy(&comm);

  g_pVBoxFuncs->pfnClientUninitialize();
  VBoxCGlueTerm();

  exit(status);
}

void err_exit(char *err) {
  log_err(err);
  _exit(1);
}

/*int main(int argc, char *argv[]) {*/
    /*// Start Machine*/
    /*[>start_vm(machine);<]*/
    /*IProgress *progress;*/
    /*BSTR type, env;*/
    /*g_pVBoxFuncs->pfnUtf8ToUtf16("gui", &type);*/
    /*rc = IMachine_LaunchVMProcess(machine, session, type, env, &progress);*/
    /*g_pVBoxFuncs->pfnUtf16Free(type);*/
    /*sleep(20);*/

    /*// Get console for machine*/
    /*IConsole *console;*/
    /*ISession_get_Console(session, &console);*/


    /*// Teleport*/
/*[>    BSTR hostnameu, passu;<]*/
    /*[>g_pVBoxFuncs->pfnUtf8ToUtf16("192.168.1.102", &hostnameu);<]*/
    /*[>g_pVBoxFuncs->pfnUtf8ToUtf16("123", &passu);<]*/
    /*[>rc = IConsole_Teleport(console, hostnameu, 4884, passu, 500);<]*/
    /*[>g_pVBoxFuncs->pfnUtf16Free(hostnameu);<]*/

    /*_exit(0);*/
/*}*/
