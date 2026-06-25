/*
 * file_demo.c -- POSIX file creation, permissions (rwx) and access control
 * (Task 3, the literal 3.1.1 - 3.1.3).
 *
 * OS/systems primitive: file I/O and the POSIX permission model
 * (owner / group / other, each with read-write-execute bits), demonstrated
 * with open(), chmod(), access(), and the kernel's enforcement of those bits
 * on open().
 *
 * The file is left on disk afterwards so it can also be inspected by hand
 * (`ls -l`, `cat`, `nano`) -- exactly the manual check the brief describes.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

#define PATH "/tmp/durakv_fileperm.txt"

/* render st_mode as the familiar "rw-r--r--" string, like `ls -l` */
static void show_perms(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) { perror("stat"); return; }
    mode_t m = st.st_mode;
    char s[10];
    s[0] = (m & S_IRUSR) ? 'r' : '-';  s[1] = (m & S_IWUSR) ? 'w' : '-';
    s[2] = (m & S_IXUSR) ? 'x' : '-';  s[3] = (m & S_IRGRP) ? 'r' : '-';
    s[4] = (m & S_IWGRP) ? 'w' : '-';  s[5] = (m & S_IXGRP) ? 'x' : '-';
    s[6] = (m & S_IROTH) ? 'r' : '-';  s[7] = (m & S_IWOTH) ? 'w' : '-';
    s[8] = (m & S_IXOTH) ? 'x' : '-';  s[9] = '\0';
    printf("  perms: %s  (octal %04o)\n", s, (unsigned)(m & 07777));
}

static void try_write(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_APPEND);
    if (fd < 0) { printf("  write -> DENIED (%s)\n", strerror(errno)); return; }
    write(fd, text, strlen(text));
    close(fd);
    printf("  write -> ALLOWED\n");
}

static void try_read(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { printf("  read  -> DENIED (%s)\n", strerror(errno)); return; }
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n >= 0) { buf[n] = '\0'; printf("  read  -> ALLOWED: \"%.*s\"\n",
                                        (int)strcspn(buf, "\n"), buf); }
    close(fd);
}

int main(void)
{
    unlink(PATH);

    /* 3.1.1 -- create a file and set its permissions (rw-r--r--) */
    printf("1. create %s with mode 0644 (owner rw, group/other read-only)\n", PATH);
    int fd = open(PATH, O_CREAT | O_WRONLY, 0644);
    if (fd < 0) { perror("open(create)"); return 1; }
    const char *content = "durakv file-permission demo\n";
    write(fd, content, strlen(content));
    close(fd);
    show_perms(PATH);

    /* 3.1.2 -- query the caller's access rights */
    printf("\n2. access() rights for the calling user:\n");
    printf("  R_OK (read):    %s\n", access(PATH, R_OK) == 0 ? "yes" : "no");
    printf("  W_OK (write):   %s\n", access(PATH, W_OK) == 0 ? "yes" : "no");
    printf("  X_OK (execute): %s\n", access(PATH, X_OK) == 0 ? "yes" : "no");

    /* 3.1.3 -- change permissions; the kernel enforces them on open() */
    printf("\n3. chmod 0444 (read-only): reads succeed, writes are refused\n");
    chmod(PATH, 0444);
    show_perms(PATH);
    try_read(PATH);
    try_write(PATH, "this must be rejected\n");

    printf("\n4. chmod 0644: writing is permitted again\n");
    chmod(PATH, 0644);
    show_perms(PATH);
    try_write(PATH, "appended after re-enabling write\n");

    printf("\n5. chmod 0600 (owner-only): group and others lose all access\n");
    chmod(PATH, 0600);
    show_perms(PATH);

    printf("\nFile left at %s -- verify by hand:  ls -l %s   |   cat %s\n",
           PATH, PATH, PATH);
    printf("\nfile_demo: done\n");
    return 0;
}
