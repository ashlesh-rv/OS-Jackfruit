/* KEEP YOUR ORIGINAL CODE ABOVE SAME — ONLY REPLACE TODO PARTS BELOW */

/* ================= IMPLEMENTATIONS ================= */

int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);

    return 0;
}

int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);

    return 0;
}

void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    mkdir(LOG_DIR, 0755);

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);

        FILE *fp = fopen(path, "a");
        if (fp) {
            fwrite(item.data, 1, item.length, fp);
            fclose(fp);
        }
    }

    return NULL;
}

int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    if (chroot(cfg->rootfs) != 0) {
        perror("chroot failed");
        return 1;
    }

    chdir("/");

    mount("proc", "/proc", "proc", 0, NULL);

    dup2(cfg->log_write_fd, STDOUT_FILENO);
    dup2(cfg->log_write_fd, STDERR_FILENO);

    execl(cfg->command, cfg->command, NULL);

    perror("exec failed");
    return 1;
}

static int send_control_request(const control_request_t *req)
{
    if (req->kind == CMD_RUN) {

        int pipefd[2];
        pipe(pipefd);

        pid_t pid = fork();

        if (pid == 0) {
            close(pipefd[0]);

            child_config_t cfg;
            memset(&cfg, 0, sizeof(cfg));

            strncpy(cfg.id, req->container_id, CONTAINER_ID_LEN - 1);
            strncpy(cfg.rootfs, req->rootfs, PATH_MAX - 1);
            strncpy(cfg.command, req->command, CHILD_COMMAND_LEN - 1);

            cfg.log_write_fd = pipefd[1];

            void *stack = malloc(STACK_SIZE);
            if (!stack) {
                perror("malloc failed");
                exit(1);
            }

            if (clone(child_fn, stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWNS | SIGCHLD, &cfg) < 0) {
                perror("clone failed");
                exit(1);
            }

            exit(0);
        } else if (pid > 0) {
            close(pipefd[1]);

            printf("Container %s started with PID %d\n",
                   req->container_id, pid);

            return 0;
        } else {
            perror("fork failed");
            return 1;
        }
    }

    printf("Command not implemented fully yet\n");
    return 0;
}
