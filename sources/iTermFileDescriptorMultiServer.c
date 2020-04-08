//
//  iTermFileDescriptorMultiServer.c
//  iTerm2
//
//  Created by George Nachman on 7/22/19.
//

#include "iTermFileDescriptorMultiServer.h"

#include "iTermCLogging.h"
#include "iTermFileDescriptorServerShared.h"

#include <Carbon/Carbon.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/un.h>

#ifndef ITERM_SERVER
#error ITERM_SERVER not defined. Build process is broken.
#endif

const char *gMultiServerSocketPath;

// On entry there should be three file descriptors:
// 0: A socket we can accept() on. listen() was already called on it.
// 1: A connection we can sendmsg() on. accept() was already called on it.
// 2: A pipe that can be used to detect this process's termination. Do nothing with it.
// 3: A pipe we can recvmsg() on.
typedef enum {
    iTermMultiServerFileDescriptorAcceptSocket = 0,
    iTermMultiServerFileDescriptorInitialWrite = 1,
    iTermMultiServerFileDescriptorDeadMansPipe = 2,
    iTermMultiServerFileDescriptorInitialRead = 3
} iTermMultiServerFileDescriptor;

static int MakeBlocking(int fd);

#import "iTermFileDescriptorServer.h"
#import "iTermMultiServerProtocol.h"
#import "iTermPosixTTYReplacements.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <util.h>

static int gPipe[2];
static char *gPath;

typedef struct {
    iTermMultiServerClientOriginatedMessage messageWithLaunchRequest;
    pid_t pid;
    int terminated;  // Nonzero if process is terminated and wait()ed on.
    int willTerminate;  // Preemptively terminated. Stop reporting its existence.
    int masterFd;  // Valid only if !terminated && !willTerminate
    int status;  // Only valid if terminated. Gives status from wait.
    const char *tty;
} iTermMultiServerChild;

static iTermMultiServerChild *children;
static int numberOfChildren;

#pragma mark - Signal handlers

static void SigChildHandler(int arg) {
    // Wake the select loop.
    write(gPipe[1], "", 1);
}

#pragma mark - Inspect Children

static int GetNumberOfReportableChildren(void) {
    int n = 0;
    for (int i = 0; i < numberOfChildren; i++) {
        if (children[i].willTerminate) {
            continue;
        }
        n++;
    }
    return n;
}

#pragma mark - Mutate Children

static void LogChild(const iTermMultiServerChild *child) {
    FDLog(LOG_DEBUG, "masterFd=%d, pid=%d, willTerminate=%d, terminated=%d, status=%d, tty=%s", child->masterFd, child->pid, child->willTerminate, child->terminated, child->status, child->tty ?: "(null)");
}

static void AddChild(const iTermMultiServerRequestLaunch *launch,
                     int masterFd,
                     const char *tty,
                     const iTermForkState *forkState) {
    if (!children) {
        children = calloc(1, sizeof(iTermMultiServerChild));
    } else {
        children = realloc(children, (numberOfChildren + 1) * sizeof(iTermMultiServerChild));
    }
    const int i = numberOfChildren;
    numberOfChildren += 1;
    iTermMultiServerClientOriginatedMessage tempClientMessage = {
        .type = iTermMultiServerRPCTypeLaunch,
        .payload = {
            .launch = *launch
        }
    };

    // Copy the launch request into children[i].messageWithLaunchRequest. This is done because we
    // need to own our own pointers to arrays of strings.
    iTermClientServerProtocolMessage tempMessage;
    iTermClientServerProtocolMessageInitialize(&tempMessage);
    int status;
    status = iTermMultiServerProtocolEncodeMessageFromClient(&tempClientMessage, &tempMessage);
    assert(status == 0);
    status = iTermMultiServerProtocolParseMessageFromClient(&tempMessage,
                                                            &children[i].messageWithLaunchRequest);
    assert(status == 0);
    iTermClientServerProtocolMessageFree(&tempMessage);

    // Update for the remaining fields in children[i].
    children[i].masterFd = masterFd;
    children[i].pid = forkState->pid;
    children[i].willTerminate = 0;
    children[i].terminated = 0;
    children[i].status = 0;
    children[i].tty = strdup(tty);

    FDLog(LOG_DEBUG, "Added child %d:", i);
    LogChild(&children[i]);
}

static void FreeChild(int i) {
    assert(i >= 0);
    assert(i < numberOfChildren);
    FDLog(LOG_DEBUG, "Free child %d", i);
    iTermMultiServerChild *child = &children[i];
    free((char *)child->tty);
    iTermMultiServerClientOriginatedMessageFree(&child->messageWithLaunchRequest);
    child->tty = NULL;
}

static void RemoveChild(int i) {
    assert(i >= 0);
    assert(i < numberOfChildren);

    FDLog(LOG_DEBUG, "Remove child %d", i);
    if (numberOfChildren == 1) {
        free(children);
        children = NULL;
    } else {
        FreeChild(i);
        const int afterCount = numberOfChildren - i - 1;
        memmove(children + i,
                children + i + 1,
                sizeof(*children) * afterCount);
        children = realloc(children, sizeof(*children) * (numberOfChildren - 1));
    }

    numberOfChildren -= 1;
}

#pragma mark - Launch

static int Launch(const iTermMultiServerRequestLaunch *launch,
                  iTermForkState *forkState,
                  iTermTTYState *ttyStatePtr,
                  int *errorPtr) {
    iTermTTYStateInit(ttyStatePtr,
                      iTermTTYCellSizeMake(launch->columns, launch->rows),
                      iTermTTYPixelSizeMake(launch->pixel_width, launch->pixel_height),
                      launch->isUTF8);
    int fd;
    forkState->numFileDescriptorsToPreserve = 3;
    FDLog(LOG_DEBUG, "Forking...");
    forkState->pid = forkpty(&fd, ttyStatePtr->tty, &ttyStatePtr->term, &ttyStatePtr->win);
    if (forkState->pid == (pid_t)0) {
        // Child
        iTermExec(launch->path,
                  (const char **)launch->argv,
                  1,  /* close file descriptors */
                  0,  /* restore resource limits */
                  forkState,
                  launch->pwd,
                  launch->envp,
                  fd);
    }
    if (forkState->pid == 1) {
        *errorPtr = errno;
        FDLog(LOG_DEBUG, "forkpty failed: %s", strerror(errno));
        return -1;
    } 
    FDLog(LOG_DEBUG, "forkpty succeeded. Child pid is %d", forkState->pid);
    *errorPtr = 0;
    return fd;
}

static int SendLaunchResponse(int fd, int status, pid_t pid, int masterFd, const char *tty, unsigned long long uniqueId) {
    iTermClientServerProtocolMessage obj;
    iTermClientServerProtocolMessageInitialize(&obj);

    iTermMultiServerServerOriginatedMessage message = {
        .type = iTermMultiServerRPCTypeLaunch,
        .payload = {
            .launch = {
                .status = status,
                .pid = pid,
                .uniqueId = uniqueId,
                .tty = tty
            }
        }
    };
    const int rc = iTermMultiServerProtocolEncodeMessageFromServer(&message, &obj);
    if (rc) {
        FDLog(LOG_ERR, "Error encoding launch response");
        return -1;
    }

    ssize_t result;
    if (masterFd >= 0) {
        // Happy path. Send the file descriptor.
        FDLog(LOG_DEBUG, "NOTE: sending file descriptor");
        result = iTermFileDescriptorServerSendMessageAndFileDescriptor(fd,
                                                                       obj.ioVectors[0].iov_base,
                                                                       obj.ioVectors[0].iov_len,
                                                                       masterFd);
    } else {
        // Error happened. Don't send a file descriptor.
        FDLog(LOG_ERR, "ERROR: *not* sending file descriptor");
        int error;
        result = iTermFileDescriptorServerSendMessage(fd,
                                                      obj.ioVectors[0].iov_base,
                                                      obj.ioVectors[0].iov_len,
                                                      &error);
        if (result < 0) {
            FDLog(LOG_ERR, "SendMsg failed with %s", strerror(error));
        }
    }
    iTermClientServerProtocolMessageFree(&obj);
    return result == -1;
}

static int HandleLaunchRequest(int fd, const iTermMultiServerRequestLaunch *launch) {
    FDLog(LOG_DEBUG, "HandleLaunchRequest fd=%d", fd);

    iTermForkState forkState = {
        .connectionFd = -1,
        .deadMansPipe = { 0, 0 },
    };
    iTermTTYState ttyState;
    memset(&ttyState, 0, sizeof(ttyState));

    int error = 0;
    int masterFd = Launch(launch, &forkState, &ttyState, &error);
    if (masterFd < 0) {
        return SendLaunchResponse(fd,
                                  -1 /* status */,
                                  0 /* pid */,
                                  -1 /* masterFd */,
                                  "" /* tty */,
                                  launch->uniqueId);
    }

    // Happy path
    AddChild(launch, masterFd, ttyState.tty, &forkState);
    return SendLaunchResponse(fd,
                              0 /* status */,
                              forkState.pid,
                              masterFd,
                              ttyState.tty,
                              launch->uniqueId);
}

#pragma mark - Report Termination

static int ReportTermination(int fd, pid_t pid) {
    FDLog(LOG_DEBUG, "Report termination pid=%d fd=%d", (int)pid, fd);

    iTermClientServerProtocolMessage obj;
    iTermClientServerProtocolMessageInitialize(&obj);

    iTermMultiServerServerOriginatedMessage message = {
        .type = iTermMultiServerRPCTypeTermination,
        .payload = {
            .termination = {
                .pid = pid,
            }
        }
    };
    const int rc = iTermMultiServerProtocolEncodeMessageFromServer(&message, &obj);
    if (rc) {
        FDLog(LOG_ERR, "Failed to encode termination report");
        return -1;
    }

    int error;
    ssize_t result = iTermFileDescriptorServerSendMessage(fd,
                                                          obj.ioVectors[0].iov_base,
                                                          obj.ioVectors[0].iov_len,
                                                          &error);
    if (result < 0) {
        FDLog(LOG_ERR, "SendMsg failed with %s", strerror(error));
    }
    iTermClientServerProtocolMessageFree(&obj);
    return result == -1;
}

#pragma mark - Report Child

static void PopulateReportChild(const iTermMultiServerChild *child, int isLast, iTermMultiServerReportChild *out) {
    iTermMultiServerReportChild temp = {
        .isLast = isLast,
        .pid = child->pid,
        .path = child->messageWithLaunchRequest.payload.launch.path,
        .argv = child->messageWithLaunchRequest.payload.launch.argv,
        .argc = child->messageWithLaunchRequest.payload.launch.argc,
        .envp = child->messageWithLaunchRequest.payload.launch.envp,
        .envc = child->messageWithLaunchRequest.payload.launch.envc,
        .isUTF8 = child->messageWithLaunchRequest.payload.launch.isUTF8,
        .pwd = child->messageWithLaunchRequest.payload.launch.pwd,
        .terminated = !!child->terminated,
        .tty = child->tty
    };
    *out = temp;
}

static int ReportChild(int fd, const iTermMultiServerChild *child, int isLast) {
    FDLog(LOG_DEBUG, "Report child fd=%d isLast=%d:", fd, isLast);
    LogChild(child);

    iTermClientServerProtocolMessage obj;
    iTermClientServerProtocolMessageInitialize(&obj);

    iTermMultiServerServerOriginatedMessage message = {
        .type = iTermMultiServerRPCTypeReportChild,
    };
    PopulateReportChild(child, isLast, &message.payload.reportChild);
    const int rc = iTermMultiServerProtocolEncodeMessageFromServer(&message, &obj);
    if (rc) {
        FDLog(LOG_ERR, "Failed to encode report child");
        return -1;
    }

    ssize_t bytes = iTermFileDescriptorServerSendMessageAndFileDescriptor(fd,
                                                                          obj.ioVectors[0].iov_base,
                                                                          obj.ioVectors[0].iov_len,
                                                                          child->masterFd);
    if (bytes < 0) {
        const int theError = errno;
        FDLog(LOG_ERR, "SendMsg failed with %s", strerror(theError));
        assert(theError != EAGAIN);
    } else {
        FDLog(LOG_DEBUG, "Reported child successfully");
    }
    iTermClientServerProtocolMessageFree(&obj);
    return bytes < 0;
}

#pragma mark - Termination Handling

static pid_t WaitPidNoHang(pid_t pid, int *statusOut) {
    FDLog(LOG_DEBUG, "Wait on pid %d", pid);
    pid_t result;
    do {
        result = waitpid(pid, statusOut, WNOHANG);
    } while (result < 0 && errno == EINTR);
    return result;
}

static int WaitForAllProcesses(int connectionFd) {
    FDLog(LOG_DEBUG, "WaitForAllProcesses connectionFd=%d", connectionFd);

    FDLog(LOG_DEBUG, "Emptying pipe...");
    ssize_t rc;
    do {
        char c;
        rc = read(gPipe[0], &c, sizeof(c));
    } while (rc > 0 || (rc == -1 && errno == EINTR));
    if (rc < 0 && errno != EAGAIN) {
        FDLog(LOG_ERR, "Read of gPipe[0] failed with %s", strerror(errno));
    }
    FDLog(LOG_DEBUG, "Done emptying pipe. Wait on non-terminated children.");
    for (int i = 0; i < numberOfChildren; i++) {
        if (children[i].terminated) {
            continue;
        }
        const pid_t pid = WaitPidNoHang(children[i].pid, &children[i].status);
        if (pid > 0) {
            FDLog(LOG_DEBUG, "Child with pid %d exited with status %d", (int)pid, children[i].status);
            children[i].terminated = 1;
            if (!children[i].willTerminate &&
                connectionFd >= 0 &&
                ReportTermination(connectionFd, children[i].pid)) {
                FDLog(LOG_DEBUG, "ReportTermination returned an error");
                return -1;
            }
        }
    }
    FDLog(LOG_DEBUG, "Finished making waitpid calls");
    return 0;
}

#pragma mark - Report Children

static int ReportChildren(int fd) {
    FDLog(LOG_DEBUG, "Reporting children...");
    // Iterate backwards because ReportAndRemoveDeadChild deletes the index passed to it.
    const int numberOfReportableChildren = GetNumberOfReportableChildren();
    int numberSent = 0;
    for (int i = numberOfChildren - 1; i >= 0; i--) {
        if (children[i].willTerminate) {
            continue;
        }
        if (ReportChild(fd, &children[i], numberSent + 1 == numberOfReportableChildren)) {
            FDLog(LOG_ERR, "ReportChild returned an error code");
            return -1;
        }
        numberSent += 1;
    }
    FDLog(LOG_DEBUG, "Done reporting children...");
    return 0;
}

#pragma mark - Handshake

static int HandleHandshake(int fd, iTermMultiServerRequestHandshake *handshake) {
    FDLog(LOG_DEBUG, "Handle handshake maximumProtocolVersion=%d", handshake->maximumProtocolVersion);;
    iTermClientServerProtocolMessage obj;
    iTermClientServerProtocolMessageInitialize(&obj);

    if (handshake->maximumProtocolVersion < iTermMultiServerProtocolVersion1) {
        FDLog(LOG_ERR, "Maximum protocol version is too low: %d", handshake->maximumProtocolVersion);
        return -1;
    }
    iTermMultiServerServerOriginatedMessage message = {
        .type = iTermMultiServerRPCTypeHandshake,
        .payload = {
            .handshake = {
                .protocolVersion = iTermMultiServerProtocolVersion1,
                .numChildren = GetNumberOfReportableChildren(),
                .pid = getpid()
            }
        }
    };
    const int rc = iTermMultiServerProtocolEncodeMessageFromServer(&message, &obj);
    if (rc) {
        FDLog(LOG_ERR, "Failed to encode handshake response");
        return -1;
    }

    int error;
    ssize_t bytes = iTermFileDescriptorServerSendMessage(fd,
                                                         obj.ioVectors[0].iov_base,
                                                         obj.ioVectors[0].iov_len,
                                                         &error);
    if (bytes < 0) {
        FDLog(LOG_ERR, "SendMsg failed with %s", strerror(error));
    }

    iTermClientServerProtocolMessageFree(&obj);
    if (bytes < 0) {
        return -1;
    }
    return ReportChildren(fd);
}

#pragma mark - Wait

static int GetChildIndexByPID(pid_t pid) {
    for (int i = 0; i < numberOfChildren; i++) {
        if (children[i].pid == pid) {
            return i;
        }
    }
    return -1;
}

static int HandleWait(int fd, iTermMultiServerRequestWait *wait) {
    FDLog(LOG_DEBUG, "Handle wait request for pid=%d preemptive=%d", wait->pid, wait->removePreemptively);

    iTermClientServerProtocolMessage obj;
    iTermClientServerProtocolMessageInitialize(&obj);

    int childIndex = GetChildIndexByPID(wait->pid);
    int status = 0;
    int errorNumber = 0;
    if (childIndex < 0) {
        errorNumber = -1;
    } else if (!children[childIndex].terminated) {
        if (wait->removePreemptively) {
            children[childIndex].willTerminate = 1;
            close(children[childIndex].masterFd);
            children[childIndex].masterFd = -1;
            status = 0;
            errorNumber = 1;
        } else {
            errorNumber = -2;
        }
    } else {
        status = children[childIndex].status;
    }
    iTermMultiServerServerOriginatedMessage message = {
        .type = iTermMultiServerRPCTypeWait,
        .payload = {
            .wait = {
                .pid = wait->pid,
                .status = status,
                .errorNumber = errorNumber
            }
        }
    };
    const int rc = iTermMultiServerProtocolEncodeMessageFromServer(&message, &obj);
    if (rc) {
        FDLog(LOG_ERR, "Failed to encode wait response");
        return -1;
    }

    int error;
    ssize_t bytes = iTermFileDescriptorServerSendMessage(fd,
                                                         obj.ioVectors[0].iov_base,
                                                         obj.ioVectors[0].iov_len,
                                                         &error);
    if (bytes < 0) {
        FDLog(LOG_ERR, "SendMsg failed with %s", strerror(error));
    }

    iTermClientServerProtocolMessageFree(&obj);
    if (bytes < 0) {
        return -1;
    }

    if (errorNumber == 0) {
        RemoveChild(childIndex);
    }
    return 0;
}

#pragma mark - Requests

static int ReadRequest(int fd, iTermMultiServerClientOriginatedMessage *out) {
    iTermClientServerProtocolMessage message;
    FDLog(LOG_DEBUG, "Reading a request...");
    int status = iTermMultiServerRead(fd, &message);
    if (status) {
        FDLog(LOG_DEBUG, "Read failed");
        goto done;
    }

    memset(out, 0, sizeof(*out));

    status = iTermMultiServerProtocolParseMessageFromClient(&message, out);
    if (status) {
        FDLog(LOG_ERR, "Parse failed with status %d", status);
    } else {
        FDLog(LOG_DEBUG, "Parsed message from client:");
        iTermMultiServerProtocolLogMessageFromClient(out);
    }
    iTermClientServerProtocolMessageFree(&message);

done:
    if (status) {
        iTermMultiServerClientOriginatedMessageFree(out);
    }
    return status;
}

static int ReadAndHandleRequest(int readFd, int writeFd) {
    iTermMultiServerClientOriginatedMessage request;
    if (ReadRequest(readFd, &request)) {
        return -1;
    }
    FDLog(LOG_DEBUG, "Handle request of type %d", (int)request.type);
    iTermMultiServerProtocolLogMessageFromClient(&request);
    int result = 0;
    switch (request.type) {
        case iTermMultiServerRPCTypeHandshake:
            result = HandleHandshake(writeFd, &request.payload.handshake);
            break;
        case iTermMultiServerRPCTypeWait:
            result = HandleWait(writeFd, &request.payload.wait);
            break;
        case iTermMultiServerRPCTypeLaunch:
            result = HandleLaunchRequest(writeFd, &request.payload.launch);
            break;
        case iTermMultiServerRPCTypeTermination:
            FDLog(LOG_ERR, "Ignore termination message");
            break;
        case iTermMultiServerRPCTypeReportChild:
            FDLog(LOG_ERR, "Ignore report child message");
            break;
    }
    iTermMultiServerClientOriginatedMessageFree(&request);
    return 0;
}

#pragma mark - Core

static void AcceptAndReject(int socket) {
    FDLog(LOG_DEBUG, "Calling accept()...");
    int fd = iTermFileDescriptorServerAccept(socket);
    if (fd < 0) {
        FDLog(LOG_ERR, "Don't send message: accept failed");
        return;
    }

    FDLog(LOG_DEBUG, "Received connection attempt while already connected. Send rejection.");

    iTermMultiServerServerOriginatedMessage message = {
        .type = iTermMultiServerRPCTypeHandshake,
        .payload = {
            .handshake = {
                .protocolVersion = iTermMultiServerProtocolVersionRejected,
                .numChildren = 0,
            }
        }
    };
    iTermClientServerProtocolMessage obj;
    iTermClientServerProtocolMessageInitialize(&obj);
    const int rc = iTermMultiServerProtocolEncodeMessageFromServer(&message, &obj);
    if (rc) {
        FDLog(LOG_ERR, "Failed to encode version-rejected");
        goto done;
    }
    int error;
    const ssize_t result = iTermFileDescriptorServerSendMessage(fd,
                                                                obj.ioVectors[0].iov_base,
                                                                obj.ioVectors[0].iov_len,
                                                                &error);
    if (result < 0) {
        FDLog(LOG_ERR, "SendMsg failed with %s", strerror(error));
    }

    iTermClientServerProtocolMessageFree(&obj);

done:
    close(fd);
}

// There is a client connected. Respond to requests from it until it disconnects, then return.
static void SelectLoop(int acceptFd, int writeFd, int readFd) {
    FDLog(LOG_DEBUG, "Begin SelectLoop.");
    while (1) {
        static const int fdCount = 3;
        int fds[fdCount] = { gPipe[0], acceptFd, readFd };
        int results[fdCount];
        FDLog(LOG_DEBUG, "Calling select()");
        iTermSelect(fds, sizeof(fds) / sizeof(*fds), results, 1 /* wantErrors */);

        if (results[2]) {
            // readFd
            FDLog(LOG_DEBUG, "select: have data to read");
            if (ReadAndHandleRequest(readFd, writeFd)) {
                FDLog(LOG_DEBUG, "ReadAndHandleRequest returned failure code.");
                if (results[0]) {
                    FDLog(LOG_DEBUG, "Client hung up and also have SIGCHLD to deal with. Wait for processes.");
                    WaitForAllProcesses(-1);
                }
                break;
            }
        }
        if (results[0]) {
            // gPipe[0]
            FDLog(LOG_DEBUG, "select: SIGCHLD happened during select");
            if (WaitForAllProcesses(writeFd)) {
                break;
            }
        }
        if (results[1]) {
            // socketFd
            FDLog(LOG_DEBUG, "select: socket is readable");
            AcceptAndReject(acceptFd);
        }
    }
    FDLog(LOG_DEBUG, "Exited select loop.");
    close(writeFd);
}

static int MakeAndSendPipe(int unixDomainSocketFd) {
    int fds[2];
    if (pipe(fds) != 0) {
        return -1;
    }

    int readPipe = fds[0];
    int writePipe = fds[1];

    const ssize_t rc = iTermFileDescriptorServerSendMessageAndFileDescriptor(unixDomainSocketFd, "", 0, writePipe);
    if (rc == -1) {
        FDLog(LOG_ERR, "Failed to send write file descriptor: %s", strerror(errno));
        close(readPipe);
        readPipe = -1;
    }

    FDLog(LOG_DEBUG, "Sent write end of pipe");
    close(writePipe);
    return readPipe;
}

static int iTermMultiServerAccept(int socketFd) {
    // incoming unix domain socket connection to get FDs
    int connectionFd = -1;
    while (1) {
        int fds[] = { socketFd, gPipe[0] };
        int results[2] = { 0, 0 };
        FDLog(LOG_DEBUG, "iTermMultiServerAccept calling iTermSelect...");
        iTermSelect(fds, sizeof(fds) / sizeof(*fds), results, 1);
        FDLog(LOG_DEBUG, "iTermSelect returned.");
        if (results[1]) {
            FDLog(LOG_DEBUG, "SIGCHLD pipe became readable while waiting for connection. Calling wait...");
            WaitForAllProcesses(-1);
            FDLog(LOG_DEBUG, "Done wait()ing on all children");
        }
        if (results[0]) {
            FDLog(LOG_DEBUG, "Socket became readable. Calling accept()...");
            connectionFd = iTermFileDescriptorServerAccept(socketFd);
            if (connectionFd != -1) {
                break;
            }
        }
        FDLog(LOG_DEBUG, "accept() returned %d error=%s", connectionFd, strerror(errno));
    }
    return connectionFd;
}

// Alternates between running the select loop and accepting a new connection.
static void MainLoop(char *path, int acceptFd, int initialWriteFd, int initialReadFd) {
    FDLog(LOG_DEBUG, "Entering main loop.");
    assert(acceptFd >= 0);
    assert(acceptFd != initialWriteFd);
    assert(initialWriteFd >= 0);
    assert(initialReadFd >= 0);

    int writeFd = initialWriteFd;
    int readFd = initialReadFd;
    MakeBlocking(writeFd);
    MakeBlocking(readFd);

    do {
        if (writeFd >= 0 && readFd >= 0) {
            SelectLoop(acceptFd, writeFd, readFd);
        }

        if (GetNumberOfReportableChildren() == 0) {
            // Not attached and no children? Quit rather than leave a useless daemon running.
            FDLog(LOG_DEBUG, "Exiting because no reportable children remain. %d terminating.", numberOfChildren);
            return;
        }

        // You get here after the connection is lost. Listen and accept.
        FDLog(LOG_DEBUG, "Calling iTermMultiServerAccept");
        writeFd = iTermMultiServerAccept(acceptFd);
        if (writeFd == -1) {
            FDLog(LOG_ERR, "iTermMultiServerAccept failed: %s", strerror(errno));
            break;
        }
        FDLog(LOG_DEBUG, "Accept returned a valid file descriptor %d", writeFd);
        readFd = MakeAndSendPipe(writeFd);
        MakeBlocking(writeFd);
        MakeBlocking(readFd);
    } while (writeFd >= 0 && readFd >= 0);
    FDLog(LOG_DEBUG, "Returning from MainLoop because of an error.");
}

#pragma mark - Bootstrap

static int MakeNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    int rc = 0;
    do {
        rc = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    } while (rc == -1 && errno == EINTR);
    return rc == -1;
}

static int MakeBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    int rc = 0;
    do {
        rc = fcntl(fd, F_SETFL, flags & (~O_NONBLOCK));
    } while (rc == -1 && errno == EINTR);
    FDLog(LOG_DEBUG, "MakeBlocking(%d) returned %d (%s)", fd, rc, strerror(errno));
    return rc == -1;
}

static int MakeStandardFileDescriptorsNonBlocking(void) {
    int status = 1;

    if (MakeNonBlocking(iTermMultiServerFileDescriptorAcceptSocket)) {
        goto done;
    }
    if (MakeBlocking(iTermMultiServerFileDescriptorInitialWrite)) {
        goto done;
    }
    if (MakeBlocking(iTermMultiServerFileDescriptorDeadMansPipe)) {
        goto done;
    }
    if (MakeBlocking(iTermMultiServerFileDescriptorInitialRead)) {
        goto done;
    }
    status = 0;

done:
    return status;
}

static int MakePipe(void) {
    if (pipe(gPipe) < 0) {
        FDLog(LOG_ERR, "Failed to create pipe: %s", strerror(errno));
        return 1;
    }

    // Make pipes nonblocking
    for (int i = 0; i < 2; i++) {
        if (MakeNonBlocking(gPipe[i])) {
            FDLog(LOG_ERR, "Failed to set gPipe[%d] nonblocking: %s", i, strerror(errno));
            return 2;
        }
    }
    return 0;
}

static int InitializeSignals(void) {
    // We get this when iTerm2 crashes. Ignore it.
    FDLog(LOG_DEBUG, "Installing SIGHUP handler.");
    sig_t rc = signal(SIGHUP, SIG_IGN);
    if (rc == SIG_ERR) {
        FDLog(LOG_ERR, "signal(SIGHUP, SIG_IGN) failed with %s", strerror(errno));
        return 1;
    }

    // Unblock SIGCHLD.
    sigset_t signal_set;
    sigemptyset(&signal_set);
    sigaddset(&signal_set, SIGCHLD);
    FDLog(LOG_DEBUG, "Unblocking SIGCHLD.");
    if (sigprocmask(SIG_UNBLOCK, &signal_set, NULL) == -1) {
        FDLog(LOG_ERR, "sigprocmask(SIG_UNBLOCK, &signal_set, NULL) failed with %s", strerror(errno));
        return 1;
    }

    FDLog(LOG_DEBUG, "Installing SIGCHLD handler.");
    rc = signal(SIGCHLD, SigChildHandler);
    if (rc == SIG_ERR) {
        FDLog(LOG_ERR, "signal(SIGCHLD, SigChildHandler) failed with %s", strerror(errno));
        return 1;
    }

    FDLog(LOG_DEBUG, "signals initialized");
    return 0;
}

static void InitializeLogging(void) {
    openlog("iTerm2-Server", LOG_PID | LOG_NDELAY, LOG_USER);
    setlogmask(LOG_UPTO(LOG_DEBUG));
}

static int Initialize(char *path) {
    InitializeLogging();

    FDLog(LOG_DEBUG, "Server starting Initialize()");

    if (MakeStandardFileDescriptorsNonBlocking()) {
        return 1;
    }

    gPath = strdup(path);

    if (MakePipe()) {
        return 1;
    }

    if (InitializeSignals()) {
        return 1;
    }

    return 0;
}

static int iTermFileDescriptorMultiServerRun(char *path, int socketFd, int writeFD, int readFD) {
    // TODO: Consider calling daemon() or its equivalent pthread_spawn at this point.

    // Use GetCurrentProcess() to force a connection to the window server. This gets us killed on
    // log out. Child process become broken because their Aqua namespace session has disappeared.
    // For example, `whoami` will print a number instead of a name. Better to die than live less
    // than your best life.
    //
    // For more on these mysteries see:
    // Technical Note TN2083 - Daemons and Agents
    //   http://mirror.informatimago.com/next/developer.apple.com/technotes/tn2005/tn2083.html
    //
    // There is also an informative comment in shell_launcher.c.
    //
    // The approach taken in shell_launcher.c, to move the process from the Aqua per-session
    // namespace to the per-user namespace was clever but had a lot of unintended consequences.
    // For example, it broke PAM hacks that let you use touch ID for sudo. It broke launching
    // Cocoa apps from the command line (sometimes! Not for me, but for the guy next to me at work).
    // The costs of "random things don't work sometimes" is higher than the benefit of sessions
    // surviving log out-log in.
    //
    // GetCurrentProcess() is deprecated, and Apple wants you to use
    // [NSRunningApplication currentApplication], instead. I don't want to take on the overhead of
    // the obj-c runtime, so I'll keep using this deprecated API until it actually breaks.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    ProcessSerialNumber psn;
    GetCurrentProcess(&psn);
#pragma clang diagnostic pop

    // Do this to remove the dock icon.
    TransformProcessType(&psn, kProcessTransformToUIElementApplication);

    SetRunningServer();
    // If iTerm2 dies while we're blocked in sendmsg we get a deadly sigpipe.
    signal(SIGPIPE, SIG_IGN);
    int rc = Initialize(path);
    if (rc) {
        FDLog(LOG_ERR, "Initialize failed with code %d", rc);
    } else {
        MainLoop(path, socketFd, writeFD, readFD);
        // MainLoop never returns, except by dying on a signal.
    }
    FDLog(LOG_DEBUG, "Cleaning up to exit");
    FDLog(LOG_DEBUG, "Unlink %s", path);
    unlink(path);
    return 1;
}


// There should be a single command-line argument, which is the path to the unix-domain socket
// I'll use.
int main(int argc, char *argv[]) {
    assert(argc == 2);
    gMultiServerSocketPath = argv[1];
    iTermFileDescriptorMultiServerRun(argv[1],
                                      iTermMultiServerFileDescriptorAcceptSocket,
                                      iTermMultiServerFileDescriptorInitialWrite,
                                      iTermMultiServerFileDescriptorInitialRead);
    return 1;
}