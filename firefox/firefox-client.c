// Jesus <GranPC>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>

#define SOCKET_PATH "/tmp/firefox_daemon.socket"

struct launch_params_t {
    char startup_token[512];
};

void fallback_to_exec(int argc, char *argv[]) {
    char *firefox_path = "/usr/lib/firefox/firefox"; // TODO: getenv("FIREFOX_PATH");
    if (firefox_path) {
        execv(firefox_path, argv);
    }

    perror("exec");
}

int main(int argc, char *argv[]) {
    int sockfd;
    struct sockaddr_un addr;
    struct launch_params_t params;

    const char *startup_token = getenv("XDG_ACTIVATION_TOKEN");
    if (startup_token) {
        strncpy(params.startup_token, startup_token, sizeof(params.startup_token));
    } else {
        params.startup_token[0] = '\0';
    }

    if ((sockfd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        fallback_to_exec(argc, argv);
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("connect");
        close(sockfd);
        fallback_to_exec(argc, argv);
        return 1;
    }

    if (write(sockfd, &params, sizeof(params)) == -1) {
        perror("write");
        close(sockfd);
        fallback_to_exec(argc, argv);
        return 1;
    }

    close(sockfd);
    printf("Launch request sent to the Firefox daemon.");

    if (argc > 1) {
        // Run the original command so it sends the already-opened instance
        // the URL to open or whatever. TODO: this causes a new tab to be
        // opened if the user was on about:blank. Oh well.
        fallback_to_exec(argc, argv);
    }

    return 0;
}
