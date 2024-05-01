// Jesus <GranPC>

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <gtk/gtk.h>

#define SOCKET_PATH "/tmp/firefox_daemon.socket"

struct launch_params_t {
    char startup_token[512];
};

struct launch_params_t *listen_for_init_messages() {
    int sockfd, client_sock;
    struct sockaddr_un addr;

    if ((sockfd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        return NULL;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path)-1);
    unlink(SOCKET_PATH);

    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("bind");
        close(sockfd);
        return NULL;
    }

    if (listen(sockfd, 1) == -1) {
        perror("listen");
        close(sockfd);
        return NULL;
    }

    client_sock = accept(sockfd, NULL, NULL);
    if (client_sock == -1) {
        perror("accept");
    }

    struct launch_params_t *params = (struct launch_params_t*)malloc(sizeof(struct launch_params_t));
    if (read(client_sock, params->startup_token, sizeof(params->startup_token)) == -1) {
        perror("read");
        close(client_sock);
        close(sockfd);
        unlink(SOCKET_PATH);
        free(params);
        return NULL;
    }

    close(client_sock);
    close(sockfd);
    unlink(SOCKET_PATH);

    return params;
}

void gtk_window_set_title (GtkWindow *window, const gchar *title) {
    static void (*original_gtk_window_set_title)(GtkWindow*, const gchar*) = NULL;
    static int was_launched = 0;

    if (getenv("FIREFOX_DAEMON_LOADED")) {
        if (!original_gtk_window_set_title) {
            original_gtk_window_set_title = dlsym(RTLD_NEXT, "gtk_window_set_title");
        }
        original_gtk_window_set_title(window, title);
        return;
    }

    printf("gtk_window_set_title(%p, \"%s\")\n", window, title);

    if (!original_gtk_window_set_title) {
        original_gtk_window_set_title = dlsym(RTLD_NEXT, "gtk_window_set_title");
    }

    if (!was_launched && title && !strcmp(title, "Mozilla Firefox")) {
        printf("gtk_window_set_title called with the initial title. Waiting for the launch request.\n");
        struct launch_params_t *params = listen_for_init_messages();
        was_launched = 1;

        printf("Launch request received. Proceeding with the app initialization. Startup token: %s\n", params->startup_token);
        setenv("XDG_ACTIVATION_TOKEN", params->startup_token, 1);
        gtk_window_set_startup_id(window, params->startup_token);
        free(params);
    }

    original_gtk_window_set_title(window, title);
}

// we don't want to load ourselves into child processes
// so we use an init function to flag that we were loaded
// and that we needn't wait for a launch request

void __attribute__((constructor)) init() {
    printf("firefox-daemon loaded\n");
    setenv("FIREFOX_DAEMON_sLOADED", "1", 1);
}