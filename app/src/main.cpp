#include "platform.hpp"

#include "msg_bus.hpp"
#include "mem_pool.hpp"
#include "buffer_pool.hpp"

#include "glavo_service.hpp"
#include "tcp_sender.hpp"
#include "device_config.hpp"

#include "ui_app.hpp"
#include "upc_sys_monitor.hpp"

#include <chrono>
#include <thread>
#include <cstdio>
#include <unistd.h>
#include <fcntl.h>
#include <csignal>
#include <sys/stat.h>

#define PID_FILE "/var/run/rongsu_ui.pid"
#define LOG_FILE "/var/log/rongsu_ui.log"

volatile sig_atomic_t g_shutdown_requested = 0;
void signal_handler(int signum) {
    printf("\nReceived signal %d, shutting down...\n", signum);
    g_shutdown_requested = 1;
}

int check_running() {
    if (access(PID_FILE, F_OK) != 0) return 0;
    
    FILE *fp = fopen(PID_FILE, "r");
    if (!fp) return 0;
    
    pid_t pid;
    if (fscanf(fp, "%d", &pid) != 1) {
        fclose(fp);
        return 0;
    }
    fclose(fp);
    
    // 检查进程是否真的存在
    if (kill(pid, 0) == 0) return pid;  // 正在运行
    return 0;  // PID文件残留，但进程已死
}

// 写入PID文件
int write_pid() {
    FILE *fp = fopen(PID_FILE, "w");
    if (!fp) return -1;
    fprintf(fp, "%d\n", getpid());
    fclose(fp);
    return 0;
}

// 删除PID文件
void remove_pid() {
    unlink(PID_FILE);
}

// 守护进程化
int daemonize() {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) exit(0);  // 父进程退出
    
    if (setsid() < 0) return -1;  // 创建新会话
    
    // 第二次fork防止获取控制终端
    pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) exit(0);
    
    // 重定向标准IO到日志
    int fd = open(LOG_FILE, O_WRONLY|O_CREAT|O_APPEND, 0644);
    if (fd >= 0) {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
    }
    // int null_fd = open("/dev/null", O_RDWR);
    // if (null_fd >= 0) {
    //     dup2(null_fd, STDIN_FILENO);
    //     dup2(null_fd, STDOUT_FILENO);
    //     dup2(null_fd, STDERR_FILENO);
    //     close(null_fd);
    // }
    
    chdir("/");
    umask(0);
    
    return 0;
}


int start_service(char daemon) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    if(daemon){
        // 检查是否已在运行
        pid_t running_pid = check_running();
        if (running_pid > 0) {
            fprintf(stderr, "Already running (PID: %d)\n", running_pid);
            return 1;  // 返回1表示已在运行
        }

        if (daemonize() < 0) {
            fprintf(stderr, "Daemonize failed\n");
            return 2;
        }

        // 写入PID
        if (write_pid() < 0) {
            fprintf(stderr, "Cannot write PID file\n");
            return 3;
        }

        printf("Starting Rongsu_ui (PID: %d)...\n", running_pid);
        
    }

    middleware::MemPool::instance().init();
    middleware::BufferPool::instance().init();
    middleware::MsgBus::instance().init();

    services::GlavoService glavo_service;
    glavo_service.init();

    auto& tcp = services::TcpSender::instance();

    while(tcp.connect_tcp(GLAVO_TCP_DEFAULT_IP, GLAVO_TCP_DEFAULT_PORT) != 0) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(TCP_RECONNECT_INTERVAL_MS));
        if(g_shutdown_requested){
            if(daemon){
                remove_pid();
            }
            return 0;
        }
    }

    while(tcp.open_serial(GLAVO_SERIAL_DEFAULT_DEVICE, GLAVO_SERIAL_DEFAULT_BAUD) != 0) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(TCP_RECONNECT_INTERVAL_MS));
    }

    tcp.start_link_thread();

    if (platform_init() != 0) {
        return 1;
    }

    ui::app_init(lv_scr_act());

    glavo_service.start();

    while (!g_shutdown_requested) {
        const uint32_t delay = lv_timer_handler();
        middleware::MsgBus::instance().process();
        middleware::MsgBus::instance().process();
        upc_sys_monitor_tick();
        std::this_thread::sleep_for(
            std::chrono::milliseconds(delay > 0 ? delay : 1));
    }

    if(daemon){
        remove_pid();
    }
    return 0; 
}

// 停止服务
int stop_service() {
    pid_t pid = check_running();
    if (pid == 0) {
        printf("Not running\n");
        return 1;  // 未运行
    }
    
    printf("Stopping Rongsu_ui (PID: %d)...\n", pid);
    
    // 发送SIGTERM信号
    if (kill(pid, SIGTERM) < 0) {
        perror("kill failed");
        return 2;
    }
    
    // 等待进程退出（最多5秒）
    int timeout = 50;
    while (timeout-- > 0) {
        if (kill(pid, 0) != 0) {
            printf("Stopped\n");
            return 0;  // 成功停止
        }
        usleep(100000);  // 100ms
    }
    
    // 强制杀死
    printf("Force killing...\n");
    kill(pid, SIGKILL);
    remove_pid();
    return 0;
}


// 查看状态
int show_status() {
    pid_t pid = check_running();
    if (pid > 0) {
        printf("Running (PID: %d)\n", pid);
        
       
        return 0;
    } else {
        printf("Not running\n");
        return 1;
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s {start|stop|status|run}\n", argv[0]);
        printf("  start   - Start in daemon mode\n");
        printf("  stop    - Stop \n");
        printf("  status  - Show server status\n");
        printf("  run     - Run in foreground (debug mode)\n");
        return 1;  // 参数错误返回1
    }
    const char *cmd = argv[1];
    int ret = 0;

    if (strcmp(cmd, "start") == 0) {
        ret = start_service(1);
    
    } else if (strcmp(cmd, "stop") == 0) {
        ret = stop_service();
        
    } else if (strcmp(cmd, "status") == 0) {
        ret = show_status();
        
    } else if (strcmp(cmd, "run") == 0) {
        // 前台运行模式（调试用）
        printf("Running in foreground mode...\n");
        ret = start_service(0);
        if (ret == 0) printf("Started successfully\n");
        else printf("Start failed (code: %d)\n", ret);
        
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        return 1;
    }

  return ret; 
}
