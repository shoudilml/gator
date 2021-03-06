/**
 * Copyright (C) ARM Limited 2010-2016. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "Sender.h"

#include <algorithm>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "Buffer.h"
#include "Logging.h"
#include "OlySocket.h"
#include "SessionData.h"

Sender::Sender(OlySocket* socket)
        : mDataSocket(socket),
          mDataFile(nullptr, fclose),
          mDataFileName(nullptr),
          mSendMutex()
{
    // Set up the socket connection
    if (socket) {
        char streamline[64] = { 0 };
        mDataSocket = socket;

        // Receive magic sequence - can wait forever
        // Streamline will send data prior to the magic sequence for legacy support, which should be ignored for v4+
        while (strcmp("STREAMLINE", streamline) != 0) {
            if (mDataSocket->receiveString(streamline, sizeof(streamline)) == -1) {
                logg.logError("Socket disconnected");
                handleException();
            }
        }

        // Send magic sequence - must be done first, after which error messages can be sent
        char magic[32];
        snprintf(magic, 32, "GATOR %i\n", PROTOCOL_VERSION);
        mDataSocket->send(magic, strlen(magic));

        gSessionData.mWaitingOnCommand = true;
        logg.logMessage("Completed magic sequence");
    }

    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0 || pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK) != 0
            || pthread_mutex_init(&mSendMutex, &attr) != 0 || pthread_mutexattr_destroy(&attr) != 0 || false) {
        logg.logError("Unable to setup mutex");
        handleException();
    }
}

Sender::~Sender()
{
    // Just close it as the client socket is on the stack
    if (mDataSocket != NULL) {
        mDataSocket->closeSocket();
        mDataSocket = NULL;
    }
}

void Sender::createDataFile(char* apcDir)
{
    if (apcDir == NULL) {
        return;
    }

    mDataFileName.reset(new char[strlen(apcDir) + 12]);
    sprintf(mDataFileName.get(), "%s/0000000000", apcDir);
    mDataFile.reset(fopen_cloexec(mDataFileName.get(), "wb"));
    if (!mDataFile) {
        logg.logError("Failed to open binary file: %s", mDataFileName.get());
        handleException();
    }
}

void Sender::writeData(const char* data, int length, int type, bool ignoreLockErrors)
{
    if (length < 0 || (data == NULL && length > 0)) {
        return;
    }

    // Multiple threads call writeData()
    if (pthread_mutex_lock(&mSendMutex) != 0) {
        if (ignoreLockErrors) {
            return;
        }
        logg.logError("pthread_mutex_lock failed");
        handleException();
    }

    // Send data over the socket connection
    if (mDataSocket) {
        // Start alarm
        const int alarmDuration = 8;
        alarm(alarmDuration);

        // Send data over the socket, sending the type and size first
        logg.logMessage("Sending data with length %d", length);
        if (type != RESPONSE_APC_DATA) {
            // type and length already added by the Collector for apc data
            unsigned char header[5];
            header[0] = type;
            Buffer::writeLEInt(header + 1, length);
            mDataSocket->send(reinterpret_cast<const char *>(&header), sizeof(header));
        }

        // 100Kbits/sec * alarmDuration sec / 8 bits/byte
        const int chunkSize = 100 * 1000 * alarmDuration / 8;
        int pos = 0;
        while (true) {
            mDataSocket->send(data + pos, std::min(length - pos, chunkSize));
            pos += chunkSize;
            if (pos >= length) {
                break;
            }

            // Reset the alarm
            alarm(alarmDuration);
            logg.logMessage("Resetting the alarm");
        }

        // Stop alarm
        alarm(0);
    }

    // Write data to disk as long as it is not meta data
    if (mDataFile && type == RESPONSE_APC_DATA) {
        logg.logMessage("Writing data with length %d", length);
        // Send data to the data file
        if (fwrite(data, 1, length, mDataFile.get()) != static_cast<size_t>(length)) {
            logg.logError("Failed writing binary file %s", mDataFileName.get());
            handleException();
        }
    }

    if (pthread_mutex_unlock(&mSendMutex) != 0) {
        logg.logError("pthread_mutex_unlock failed");
        handleException();
    }
}
