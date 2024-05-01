// Jesus <GranPC>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>

#define SOCKET_PATH "/tmp/zygote_daemon.socket"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <path_to_app> <...args>\n", argv[0]);
        return 1;
    }

    const char *app_path = argv[1];
    int sockfd;
    struct sockaddr_un addr;

    if ((sockfd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("connect");
        close(sockfd);
        return 1;
    }

    // Pass all our environment variables to the daemon. Pass count first, then each variable.
    // What is environ?
    char **environ = __environ;
    int env_count = 0;
    for (char **env = environ; *env != NULL; env++) {
        env_count++;
    }
    if (write(sockfd, &env_count, sizeof(env_count)) == -1) {
        perror("write");
        close(sockfd);
        return 1;
    }
    for (char **env = environ; *env != NULL; env++) {
        ssize_t env_len = strlen(*env);
        if (write(sockfd, &env_len, sizeof(env_len)) == -1) {
            perror("write");
            close(sockfd);
            return 1;
        }
        if (write(sockfd, *env, env_len) == -1) {
            perror("write");
            close(sockfd);
            return 1;
        }
    }

    // Send the number of arguments to the daemon, skipping our own path
    int argc_to_send = argc - 1;
    if (write(sockfd, &argc_to_send, sizeof(argc_to_send)) == -1) {
        perror("write");
        close(sockfd);
        return 1;
    }

    // Send the arguments to the daemon
    for (int i = 1; i < argc; i++) {
        ssize_t arg_len = strlen(argv[i]);
        if (write(sockfd, &arg_len, sizeof(arg_len)) == -1) {
            perror("write");
            close(sockfd);
            return 1;
        }
        if (write(sockfd, argv[i], arg_len) == -1) {
            perror("write");
            close(sockfd);
            return 1;
        }
    }

    close(sockfd);
    printf("App launch request for '%s' sent to the zygote daemon.\n", app_path);

    return 0;
}
