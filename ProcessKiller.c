/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
** mountd process killer
*/

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <ctype.h>
#include <pwd.h>
#include <stdlib.h>
#include <poll.h>
#include <sys/stat.h>
#include <signal.h>

#define LOG_TAG "ProcessKiller"
#include <cutils/log.h>

#define PATH_MAX 4096

static int ReadSymLink(const char* path, char* link)
{
    struct stat s;
    int length;

    if (lstat(path, &s) < 0)
        return 0;
    if ((s.st_mode & S_IFMT) != S_IFLNK)
        return 0;
   
    // we have a symlink    
    length = readlink(path, link, PATH_MAX - 1);
    if (length <= 0) 
        return 0;
    link[length] = 0;
    return 1;
}

static int PathMatchesMountPoint(const char* path, const char* mountPoint)
{
    int length = strlen(mountPoint);
    if (length > 1 && strncmp(path, mountPoint, length) == 0)
    {
        // we need to do extra checking if mountPoint does not end in a '/'
        if (mountPoint[length - 1] == '/')
            return 1;
        // if mountPoint does not have a trailing slash, we need to make sure
        // there is one in the path to avoid partial matches.
        return (path[length] == 0 || path[length] == '/');
    }
    
    return 0;
}

static void GetProcessName(int pid, char buffer[PATH_MAX])
{
    int fd;
    sprintf(buffer, "/proc/%d/cmdline", pid);
    fd = open(buffer, O_RDONLY);
    if (fd < 0) {
        strcpy(buffer, "???");
    } else {
        int length = read(fd, buffer, PATH_MAX - 1);
        buffer[length] = 0;
        close(fd);
    }
}

static int CheckFileDescriptorSymLinks(int pid, const char* mountPoint)
{
    DIR*    dir;
    struct dirent* de;
    int fileOpen = 0;
    char    path[PATH_MAX];
    char    link[PATH_MAX];
    int     parent_length;

    // compute path to process's directory of open files
    sprintf(path, "/proc/%d/fd", pid);
    dir = opendir(path);
    if (!dir)
        return 0;

    // remember length of the path
    parent_length = strlen(path);
    // append a trailing '/'
    path[parent_length++] = '/';
    
    while ((de = readdir(dir)) != 0 && !fileOpen) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        
        // append the file name, after truncating to parent directory
        path[parent_length] = 0;
        strcat(path, de->d_name);

        if (ReadSymLink(path, link) && PathMatchesMountPoint(link, mountPoint))
        {
            char    name[PATH_MAX];
            GetProcessName(pid, name);
            LOGE("Process %s (%d) has open file %s", name, pid, link);
            fileOpen = 1;
        }
    }

    closedir(dir);
    return fileOpen;
}

static int CheckFileMaps(int pid, const char* mountPoint)
{
    FILE*   file;
    char    buffer[PATH_MAX + 100];
    int mapOpen = 0;

    sprintf(buffer, "/proc/%d/maps", pid);
    file = fopen(buffer, "r");
    if (!file)
        return 0;
    
    while (!mapOpen && fgets(buffer, sizeof(buffer), file))
    {
        // skip to the path
        const char* path = strchr(buffer, '/');
        if (path && PathMatchesMountPoint(path, mountPoint))
        {
            char    name[PATH_MAX];
            GetProcessName(pid, name);
            LOGE("process %s (%d) has open file map for %s", name, pid, path);
            mapOpen = 1;
        }
    }
    
    fclose(file);
    return mapOpen;
}

static int CheckSymLink(int pid, const char* mountPoint, const char* name, const char* message)
{
    char    path[PATH_MAX];
    char    link[PATH_MAX];

    sprintf(path, "/proc/%d/%s", pid, name);
    if (ReadSymLink(path, link) && PathMatchesMountPoint(link, mountPoint)) 
    {
        char    name[PATH_MAX];
        GetProcessName(pid, name);
        LOGW("Process %s (%d) has %s in %s", name, pid, message, mountPoint);
        return 1;
    }
    else
        return 0;
}

static int get_pid(const char* s)
{
    int result = 0;
    while (*s) {
        if (!isdigit(*s)) return -1;
        result = 10 * result + (*s++ - '0');
    }
    return result;
}

/*
 * Hunt down processes that have files open at the given mount point.
 * action = 0 to just warn,
 * action = 1 to SIGHUP,
 * action = 2 to SIGKILL
 */
// hunt down and kill processes that have files open on the given mount point
void KillProcessesWithOpenFiles(const char* mountPoint, int action)
{
    DIR*    dir;
    struct dirent* de;

    dir = opendir("/proc");
    if (!dir) return;

    while ((de = readdir(dir)) != 0)
    {
        int killed = 0;
        // does the name look like a process ID?
        int pid = get_pid(de->d_name);
        if (pid == -1) continue;

        if (CheckFileDescriptorSymLinks(pid, mountPoint)    // check for open files
                || CheckFileMaps(pid, mountPoint)           // check for mmap()
                || CheckSymLink(pid, mountPoint, "cwd", "working directory")    // check working directory
                || CheckSymLink(pid, mountPoint, "root", "chroot")              // check for chroot()
                || CheckSymLink(pid, mountPoint, "exe", "executable path")      // check executable path
            ) 
        {
            if (action == 1) {
                LOGW("Sending SIGHUP to process %d", pid);
                kill(pid, SIGTERM);
            } else if (action == 2) {
                LOGE("Sending SIGKILL to process %d", pid);
                kill(pid, SIGKILL);
            }
        }
    }

    closedir(dir);
}        
