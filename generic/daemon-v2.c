// Jesus <GranPC>

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <dlfcn.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <pthread.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <elf.h>
#include <libelf.h>
#include <gelf.h>
#include <gtk/gtk.h>
#include <fontconfig/fontconfig.h>
#include <gdk/gdk.h>
#include <gtksourceview/gtksource.h>
// wl_display
#include <wayland-client.h>

#define SOCKET_PATH "/tmp/zygote_daemon.socket"
#define MAX_APPS 10
#define NUM_CHILD_PROCESSES 3

typedef void (*init_func_t)(void);
typedef int (*app_main_t)(int, char**);

typedef struct {
    const char* lib_path;
    const char* init_func_name;
    init_func_t init_func;
    char* child_init_funcs[8];
} library_init_info;

void *child_init_funcs[32];
int child_init_funcs_count = 0;

// Add the libraries and initialization functions
library_init_info common_libs_to_preload[] = {
#ifndef _GTK4
    {"libgtk-3.so.0", "gtk_init", NULL, NULL},
#else
    {"libgtk-4.so.1", "gtk_init", NULL, {"stash_desktop_startup_notification_id"}},
    {"libadwaita-1.so.0", NULL, NULL, NULL},
    {"libgtksourceview-5.so.0", "gtk_source_init", NULL, NULL},
#endif
    // {"libQt5Core.so.5", NULL, NULL},
    {"libpulse.so.0", "pa_threaded_mainloop_new", NULL, NULL},
    {"libdbus-1.so.3", "dbus_threads_init_default", NULL, NULL},
    {"libfontconfig.so.1", "FcInit", NULL, NULL},
    {"libsqlite3.so.0", "sqlite3_initialize", NULL, NULL},
    {"libEGL.so", NULL, NULL, NULL},
    {"/usr/lib/aarch64-linux-gnu/epiphany-browser/libephymain.so", NULL, NULL, NULL},
    {"/usr/lib/aarch64-linux-gnu/epiphany-browser/libephymisc.so", NULL, NULL, NULL},
};

// Function to preload libraries and call initialization functions
void preload_libraries(library_init_info *libs_to_preload, size_t num_libs) {
    for (size_t i = 0; i < num_libs; i++) {
        printf("Loading library '%s'\n", libs_to_preload[i].lib_path);
        void* lib_handle = dlopen(libs_to_preload[i].lib_path, RTLD_LAZY | RTLD_GLOBAL);
        if (lib_handle) {
            // printf("Loaded library '%s'\n", libs_to_preload[i].lib_path);
            if (libs_to_preload[i].init_func_name) {
                libs_to_preload[i].init_func = (init_func_t) dlsym(lib_handle, libs_to_preload[i].init_func_name);
            }
            if (libs_to_preload[i].init_func) {
                // printf("Calling init function %s for %s\n", libs_to_preload[i].init_func_name, libs_to_preload[i].lib_path);
                libs_to_preload[i].init_func();
                // printf("OK\n");
            } else if (libs_to_preload[i].init_func_name) {
                fprintf(stderr, "Failed to find initialization function '%s' in '%s'\n", libs_to_preload[i].init_func_name, libs_to_preload[i].lib_path);
            }

            if (libs_to_preload[i].child_init_funcs) {
                // These are local symbols so we need to resolve them by parsing the ELF file
                Dl_info info;
                dladdr(libs_to_preload[i].init_func, &info);

                elf_version(EV_CURRENT);

                int lib_fd = open(info.dli_fname, O_RDONLY);
                if (lib_fd == -1) {
                    perror("open");
                    continue;
                }

                Elf *lib_elf = elf_begin(lib_fd, ELF_C_READ, NULL);
                if (!lib_elf) {
                    fprintf(stderr, "Failed to open ELF file for '%s'\n", info.dli_fname);
                    continue;
                }

                size_t shstrndx;
                if (elf_getshdrstrndx(lib_elf, &shstrndx) != 0) {
                    fprintf(stderr, "Failed to get section header string table index for '%s'\n", info.dli_fname);
                    continue;
                }

                Elf_Scn *scn = NULL;
                while ((scn = elf_nextscn(lib_elf, scn)) != NULL) {
                    GElf_Shdr shdr;
                    if (gelf_getshdr(scn, &shdr) != &shdr) {
                        fprintf(stderr, "Failed to get section header for '%s'\n", info.dli_fname);
                        continue;
                    }

                    char *name = elf_strptr(lib_elf, shstrndx, shdr.sh_name);
                    if (strcmp(name, ".symtab") == 0) {
                        Elf_Data *data = elf_getdata(scn, NULL);
                        if (!data) {
                            fprintf(stderr, "Failed to get data for section '%s' in '%s'\n", name, info.dli_fname);
                            continue;
                        }

                        size_t num_symbols = shdr.sh_size / shdr.sh_entsize;
                        for (size_t j = 0; j < num_symbols; j++) {
                            GElf_Sym sym;
                            if (gelf_getsym(data, j, &sym) != &sym) {
                                fprintf(stderr, "Failed to get symbol for '%s'\n", info.dli_fname);
                                continue;
                            }

                            char *sym_name = elf_strptr(lib_elf, shdr.sh_link, sym.st_name);
                            if (sym_name && sym.st_info != STT_SECTION && sym.st_info != STT_FILE) {
                                for (int k = 0; libs_to_preload[i].child_init_funcs[k]; k++) {
                                    if (strcmp(sym_name, libs_to_preload[i].child_init_funcs[k]) == 0) {
                                        printf("Found child init function '%s' in '%s'\n", sym_name, info.dli_fname);
                                        void *child_init_func = (void *) ((char *)info.dli_fbase + sym.st_value);
                                        child_init_funcs[child_init_funcs_count++] = child_init_func;
                                    }
                                }
                            }
                        }
                    }
                }

                elf_end(lib_elf);
                close(lib_fd);
            }
        } else {
            fprintf(stderr, "Failed to load library '%s': %s\n", libs_to_preload[i].lib_path, dlerror());
        }
    }

                // Pre-render a dummy window. We do it here so that SourceView and others
                // can preload their themes and fonts and whatnot
#ifdef _GTK4
                    GtkWindow *dummy_window = GTK_WINDOW (gtk_window_new());

                    // Add an icon and a label so we preload the icon theme and fontconfig
                    GtkWidget *icon = gtk_image_new_from_icon_name("document-new");
                    GtkWidget *label = gtk_label_new("Hello, world!");
                    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
                    gtk_box_append(GTK_BOX(box), icon);
                    gtk_box_append(GTK_BOX(box), label);
                    gtk_window_set_child(dummy_window, box);
                    gtk_window_present(dummy_window);

                    // g_application_run(G_APPLICATION(app), 0, NULL);
                    gdk_display_flush(gdk_display_get_default());

                    // Preload the icon theme
                    char **icon_names = gtk_icon_theme_get_icon_names(gtk_icon_theme_get_for_display(gdk_display_get_default()));
                    if (icon_names) {
                        printf("Have %d icons\n", g_strv_length(icon_names));
                        g_strfreev(icon_names);
                    }

                    /* GtkSourceStyleSchemeManager *manager = gtk_source_style_scheme_manager_new();
                    GtkSourceStyleScheme *scheme = gtk_source_style_scheme_manager_get_scheme(manager, "classic"); */
                    // There, we preloaded the sourceview themes

                    // And destroy it
                    gtk_window_destroy(dummy_window);

#else
                /* GtkWidget *dummy_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
                gtk_widget_show(dummy_window);
                gdk_display_flush(gdk_display_get_default());

                // And destroy it
                gtk_widget_destroy(dummy_window); */
#endif

    printf("Preloading done\n");
}

int initialize_socket() {
    int sockfd;
    struct sockaddr_un addr;

    if ((sockfd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    unlink(SOCKET_PATH); // Remove the previous socket if it exists

    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("bind");
        close(sockfd);
        return -1;
    }

    if (listen(sockfd, MAX_APPS) == -1) {
        perror("listen");
        close(sockfd);
        return -1;
    }

    return sockfd;
}

typedef struct child_process {
    pid_t pid;
    int sockfd;
    bool busy;
} child_process_t;

child_process_t g_child_processes[NUM_CHILD_PROCESSES];

void sigchld_handler(int sig) {
    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
    errno = saved_errno;
}

void create_child_process(int index, char *argv[]) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair");
        exit(1);
    }

    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        close(sv[0]);
        // printf("Attach the debugger now! I am %d\n", getpid());
        // sleep(5);

        preload_libraries(common_libs_to_preload, sizeof(common_libs_to_preload) / sizeof(library_init_info));

        prctl(PR_SET_NAME, (unsigned long) "zyggy", 0, 0, 0);
        pthread_setname_np(pthread_self(), "zyggy");

        printf("Renamed myself to zyggy\n");


        while (1) {
            int argc_to_read;
            int env_count;

            ssize_t read_size = read(sv[1], &env_count, sizeof(env_count));
            if (read_size < 0) {
                perror("child read");
                break;
            }

            char *envp[env_count + 1];
            for (int i = 0; i < env_count; i++) {
                ssize_t env_len;
                read_size = read(sv[1], &env_len, sizeof(env_len));
                if (read_size < 0) {
                    perror("read");
                    break;
                }

                envp[i] = (char *)malloc(env_len + 1);
                read_size = read(sv[1], envp[i], env_len);
                if (read_size < 0) {
                    perror("read");
                    break;
                }
                envp[i][env_len] = '\0';
            }

            envp[env_count] = NULL;

            // Set our environment variables
            for (int i = 0; i < env_count; i++) {
                putenv(envp[i]);
            }

            read_size = read(sv[1], &argc_to_read, sizeof(argc_to_read));
            if (read_size < 0) {
                perror("child read");
                break;
            }

            char *argv_to_read[argc_to_read + 1];

            for (int i = 0; i < argc_to_read; i++) {
                ssize_t arg_len;
                read_size = read(sv[1], &arg_len, sizeof(arg_len));
                if (read_size < 0) {
                    perror("read");
                    break;
                }

                argv_to_read[i] = (char *)malloc(arg_len + 1);
                read_size = read(sv[1], argv_to_read[i], arg_len);
                if (read_size < 0) {
                    perror("read");
                    break;
                }
                argv_to_read[i][arg_len] = '\0';
            }

            char *app_path = argv_to_read[0];

            if (read_size > 0) {
                const char *proxy_path = "/dev/shm/zygotemp";

                // Rename ourself to the app binary
                prctl(PR_SET_NAME, app_path, 0, 0, 0);

                // Create the proxy file
                int proxy_fd = open(proxy_path, O_WRONLY | O_CREAT, 0777);
                if (proxy_fd == -1) {
                    perror("open");
                    _exit(1);
                }

                // Open the app binary
                int app_fd = open(app_path, O_RDONLY);
                if (app_fd == -1) {
                    perror("open");
                    _exit(1);
                }

                // Read the app binary and modify headers and whatnot
                char app_buffer[32768];

                ssize_t read_size;
                unsigned long read_total = 0;
                unsigned long phdr_offsets[32];
                unsigned long phdr_nums[32];
                unsigned long phdr_count = 0;
                unsigned long phdr_idx = 0;
                unsigned long dyn_offsets[32];
                unsigned long dyn_count = 0;
                unsigned long dyn_idx = 0;

                while ((read_size = read(app_fd, app_buffer, sizeof(app_buffer))) > 0) {
                    // If this is the first read, check the ELF header to see if the binary is PIE
                    if (read_total == 0) {
                        Elf64_Ehdr *ehdr = (Elf64_Ehdr *)app_buffer;
                        ehdr->e_type = ET_DYN;

                        phdr_offsets[phdr_count] = ehdr->e_phoff;
                        phdr_nums[phdr_count++] = ehdr->e_phnum;
                    }

                    // If we're reading something that can contain a program header, check for dynamic segments
                    if (read_total + read_size >= phdr_offsets[phdr_idx] && read_total < phdr_offsets[phdr_idx]) {
                        Elf64_Phdr *phdr = (Elf64_Phdr *)(app_buffer + phdr_offsets[phdr_idx]);
                        for (int i = 0; i < phdr_nums[phdr_idx]; i++) {
                            if (phdr[i].p_type == PT_DYNAMIC) {
                                Elf64_Dyn *dyn = (Elf64_Dyn *)(app_buffer + phdr[i].p_offset);
                                dyn_offsets[dyn_count++] = phdr[i].p_offset;
                            }
                        }

                        phdr_idx++;
                    }

                    // If we're reading something that can contain a dynamic segment, check for DF_1_PIE
                    if (read_total + read_size >= dyn_offsets[dyn_idx] && read_total < dyn_offsets[dyn_idx]) {
                        unsigned long offset_in_chunk = dyn_offsets[dyn_idx] - read_total;
                        Elf64_Dyn *dyn = (Elf64_Dyn *)(app_buffer + offset_in_chunk);
                        for (int j = 0; dyn[j].d_tag != DT_NULL; j++) {
                            if (dyn[j].d_tag == DT_FLAGS_1) {
                                dyn[j].d_un.d_val &= ~DF_1_PIE;
                            }
                        }

                        dyn_idx++;
                    }

                    // Write the app binary to the proxy file
                    ssize_t write_size = write(proxy_fd, app_buffer, read_size);
                    if (write_size == -1) {
                        perror("write");
                        _exit(1);
                    }

                    read_total += read_size;
                }

                // Close the app binary
                close(app_fd);

                // Close the proxy file
                close(proxy_fd);

                // Load the proxy file as a shared library
                void *app_handle = dlopen(proxy_path, RTLD_LAZY | RTLD_GLOBAL);

                if (app_handle) {
                    app_main_t app_main = (app_main_t) dlsym(app_handle, "main");
                    if (app_main) {
                        // Call all child init functions
                        for (int i = 0; i < child_init_funcs_count; i++) {
                            printf("Calling child init function %d @ %p\n", i, child_init_funcs[i]);
                            ((init_func_t)child_init_funcs[i])();
                        }

                        // Make the display default so the startup notification is sent
                        gdk_display_manager_set_default_display (gdk_display_manager_get (), gdk_display_get_default ());

                        _exit(app_main(argc_to_read, argv_to_read));
                    } else {
                        fprintf(stderr, "Failed to find 'main' in '%s'\n", app_path);
                    }
                } else {
                    fprintf(stderr, "Failed to dlopen '%s'\n", app_path);
                }

                // Fallback: if dlopen() or dlsym() failed, use execl()
                execl(app_path, app_path, NULL);
                perror("execl"); // execl only returns in case of an error
                _exit(1); // Terminate the child process
            } else {
                perror("read");
                break;
            }
        }
        close(sv[1]);
        _exit(0);
    } else if (pid > 0) {
        // Parent process
        close(sv[1]);
        g_child_processes[index].pid = pid;
        g_child_processes[index].sockfd = sv[0];
        g_child_processes[index].busy = false;
    } else {
        perror("fork");
        exit(1);
    }
}

int main(int argc, char *argv[]) {
    int sockfd, client_sock;
    struct sockaddr_un client_addr;
    socklen_t client_addr_len;
    char app_path[256];
    struct sigaction sa;
    sa.sa_flags = SA_RESTART;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }

    for (int i = 0; i < NUM_CHILD_PROCESSES; i++) {
        create_child_process(i, argv);
    }

    sockfd = initialize_socket();
    if (sockfd < 0) {
        return 1;
    }

    printf("Zygote daemon is ready to receive app launch commands.\n");

    while (1) {
        client_addr_len = sizeof(client_addr);
        client_sock = accept(sockfd, (struct sockaddr*)&client_addr, &client_addr_len);
        if (client_sock == -1) {
            perror("accept");
            continue;
        }

        // Read environment variables and buffer them to send to the child process
        int env_count;
        ssize_t read_size = read(client_sock, &env_count, sizeof(env_count));
        if (read_size < 0) {
            perror("read");
            close(client_sock);
            continue;
        }

        printf("Parent: have %d environment variables\n", env_count);

        char *envp[env_count + 1];
        for (int i = 0; i < env_count; i++) {
            ssize_t env_len;
            read_size = read(client_sock, &env_len, sizeof(env_len));
            if (read_size < 0) {
                perror("read");
                close(client_sock);
                continue;
            }

            envp[i] = (char *)malloc(env_len + 1);
            read_size = read(client_sock, envp[i], env_len);
            if (read_size < 0) {
                perror("read");
                close(client_sock);
                continue;
            }
            envp[i][env_len] = '\0';
        }
        envp[env_count] = NULL;


        // Read # of arguments
        int argc_to_read;
        read_size = read(client_sock, &argc_to_read, sizeof(argc_to_read));
        if (read_size < 0) {
            perror("read");
            close(client_sock);
            continue;
        }

        char *argv_to_read[argc_to_read + 1];

        for (int i = 0; i < argc_to_read; i++) {
            ssize_t arg_len;
            read_size = read(client_sock, &arg_len, sizeof(arg_len));
            if (read_size < 0) {
                perror("read");
                close(client_sock);
                continue;
            }

            argv_to_read[i] = (char *)malloc(arg_len + 1);
            read_size = read(client_sock, argv_to_read[i], arg_len);
            if (read_size < 0) {
                perror("read");
                close(client_sock);
                continue;
            }
            argv_to_read[i][arg_len] = '\0';
        }

        close(client_sock);
        if (read_size > 0) {
            // Find an available child process
            int child_index = -1;
            for (int i = 0; i < NUM_CHILD_PROCESSES; i++) {
                if (!g_child_processes[i].busy) {
                    child_index = i;
                    g_child_processes[i].busy = true;
                    break;
                }
            }

            if (child_index == -1) {
                fprintf(stderr, "No available child process to handle app %s\n.", app_path);
                continue;
            }

            // Forward all environment variables to the child process
            if (write(g_child_processes[child_index].sockfd, &env_count, sizeof(env_count)) == -1) {
                perror("write");
                close(g_child_processes[child_index].sockfd);
                continue;
            }

            for (int i = 0; i < env_count; i++) {
                ssize_t env_len = strlen(envp[i]);
                if (write(g_child_processes[child_index].sockfd, &env_len, sizeof(env_len)) == -1) {
                    perror("write");
                    close(g_child_processes[child_index].sockfd);
                    continue;
                }
                if (write(g_child_processes[child_index].sockfd, envp[i], env_len) == -1) {
                    perror("write");
                    close(g_child_processes[child_index].sockfd);
                    continue;
                }
            }

            // Forward all arguments to the child process. First send the number of arguments
            if (write(g_child_processes[child_index].sockfd, &argc_to_read, sizeof(argc_to_read)) == -1) {
                perror("write");
                close(g_child_processes[child_index].sockfd);
                continue;
            }

            // Send the arguments to the child process
            for (int i = 0; i < argc_to_read; i++) {
                ssize_t arg_len = strlen(argv_to_read[i]);
                if (write(g_child_processes[child_index].sockfd, &arg_len, sizeof(arg_len)) == -1) {
                    perror("write");
                    close(g_child_processes[child_index].sockfd);
                    continue;
                }
                if (write(g_child_processes[child_index].sockfd, argv_to_read[i], arg_len) == -1) {
                    perror("write");
                    close(g_child_processes[child_index].sockfd);
                    continue;
                }
            }

            // Free the memory allocated for the arguments
            for (int i = 0; i < argc_to_read; i++) {
                free(argv_to_read[i]);
            }

            // Replace used child process with a new one after 1.5 seconds
            usleep(1500000);
            create_child_process(child_index, argv);
        } else {
            perror("read");
        }

        printf("Waiting for next app launch command...\n");
    }

    close(sockfd);
    unlink(SOCKET_PATH);
    return 0;
}

/* library_init_info firefox_preload_list[] = {
    {"libGL.so.1", NULL, NULL},
    {"libGLdispatch.so.0", NULL, NULL},
    {"libGLX.so.0", NULL, NULL},
    {"libX11.so.6", NULL, NULL},
    {"libX11-xcb.so.1", NULL, NULL},
    {"libxcb.so.1", NULL, NULL},
    {"libxcb-dri2.so.0", NULL, NULL},
    {"libxcb-dri3.so.0", NULL, NULL},
    {"libxcb-glx.so.0", NULL, NULL},
    {"libxcb-present.so.0", NULL, NULL},
    {"libxcb-sync.so.1", NULL, NULL},
    {"libxshmfence.so.1", NULL, NULL},
    {"libdrm.so.2", NULL, NULL},
    {"libwayland-client.so.0", NULL, NULL},
    {"libwayland-server.so.0", NULL, NULL},
    {"libwayland-egl.so.1", NULL, NULL},
    {"libwayland-cursor.so.0", NULL, NULL},
    {"libXcomposite.so.1", NULL, NULL},
    {"libXext.so.6", NULL, NULL},
    {"libXfixes.so.3", NULL, NULL},
    {"libXrender.so.1", NULL, NULL},
    {"libXt.so.6", NULL, NULL},
    {"libXtst.so.6", NULL, NULL},
    {"libXrandr.so.2", NULL, NULL},
    {"libXcursor.so.1", NULL, NULL},
    {"libXinerama.so.1", NULL, NULL},
    {"libXi.so.6", NULL, NULL},
    {"libXss.so.1", NULL, NULL},
    {"libXxf86vm.so.1", NULL, NULL},
    {"libXdmcp.so.6", NULL, NULL},
    {"libXau.so.6", NULL, NULL},
    {"libXpm.so.4", NULL, NULL},
    {"libXmu.so.6", NULL, NULL},
    {"libXaw.so.7", NULL, NULL},
    {"libXft.so.2", NULL, NULL},
    {"libXt.so.6", NULL, NULL},
    {"libXmu.so.6", NULL, NULL},
    {"libXaw.so.7", NULL, NULL},
    {"libXft.so.2", NULL, NULL},
    {"/usr/lib/firefox/libmozsandbox.so", NULL, NULL},
    {"/usr/lib/firefox/libgkcodecs.so", NULL, NULL},
    {"/usr/lib/firefox/liblgpllibs.so", NULL, NULL},
    {"/usr/lib/firefox/libmozgtk.so", NULL, NULL},
    {"/usr/lib/firefox/libmozsqlite3.so", "sqlite3_initialize", NULL},
    {"/usr/lib/firefox/libmozwayland.so", NULL, NULL},
    {"libgtk-3.so.0", NULL, NULL},
}; */
