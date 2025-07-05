#include <iostream>
#include <vector>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <algorithm>
#include <signal.h>
#include <mutex>
#include <cstring>

volatile bool running = true;
volatile int spectatorWidth = 1920;   // Разрешение spectator по умолчанию
volatile int spectatorHeight = 1080;  // Разрешение spectator по умолчанию

// Глобальные переменные для клиента и spectator
int global_client_sock = -1;
int spectator_sock = -1;
std::mutex clients_mutex;

void signal_handler(int sig) {
    std::cout << "\nReceived signal " << sig << ". Shutting down server..." << std::endl;
    running = false;
    exit(0); // Принудительное завершение
}

void handle_spectator(int spectator_sock) {
    // Spectator только получает данные, не отправляет кадры
    char buffer[1024];
    while (running) {
        int bytes = recv(spectator_sock, buffer, sizeof(buffer), 0);
        if (bytes <= 0) {
            std::cout << "Spectator disconnected" << std::endl;
            break;
        }
        // Spectator может отправлять команды или ping/pong
    }
    
    // Очищаем spectator сокет
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        spectator_sock = -1;
    }
    close(spectator_sock);
}

void handle_screen_client(int client_sock) {
#pragma pack(push, 1)
    struct FrameHeader {
        uint32_t magic;
        uint32_t width;
        uint32_t height;
        uint32_t dataSize;
        uint32_t clientScreenWidth;  // Разрешение экрана клиента
        uint32_t clientScreenHeight; // Разрешение экрана клиента
    };
#pragma pack(pop)

    char buffer[65536];

    while (running) {
        // Получаем заголовок кадра
        FrameHeader header;
        int bytes = recv(client_sock, &header, sizeof(header), MSG_WAITALL);
        if (bytes <= 0) {
            std::cout << "Client " << client_sock << " disconnected" << std::endl;
            break;
        }

        // Преобразуем байты заголовка в порядок хоста
        header.magic = ntohl(header.magic);
        header.width = ntohl(header.width);
        header.height = ntohl(header.height);
        header.dataSize = ntohl(header.dataSize);
        header.clientScreenWidth = ntohl(header.clientScreenWidth);
        header.clientScreenHeight = ntohl(header.clientScreenHeight);

        if ((header.magic != 0x12345678 && header.magic != 0x87654321) ||
            header.width > 16384 ||
            header.height > 16384 ||
            header.dataSize == 0 ||
            header.dataSize > 32 * 1024 * 1024) // 32 МБ лимит
        {
            std::cerr << "Invalid frame header from client " << client_sock << std::endl;
            break;
        }

        // Получаем данные пикселей
        std::vector<char> pixels(header.dataSize);
        size_t totalReceived = 0;
        while (totalReceived < header.dataSize) {
            bytes = recv(client_sock, pixels.data() + totalReceived,
                header.dataSize - totalReceived, 0);
            if (bytes <= 0) {
                std::cout << "Client " << client_sock << " disconnected during data transfer" << std::endl;
                goto cleanup;
            }
            totalReceived += bytes;
        }

        // Отладочная информация
        static int frameCount = 0;
        if (++frameCount % 100 == 0) {
            std::cout << "Received frame from client " << client_sock 
                      << ": " << header.width << "x" << header.height 
                      << ", magic: 0x" << std::hex << header.magic << std::dec
                      << ", size: " << header.dataSize << std::endl;
        }

        // Отправляем данные spectator (если подключен)
        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            if (spectator_sock != -1) {
                FrameHeader netHeader;
                netHeader.magic = htonl(header.magic);
                netHeader.width = htonl(header.width);
                netHeader.height = htonl(header.height);
                netHeader.dataSize = htonl(header.dataSize);
                netHeader.clientScreenWidth = htonl(header.clientScreenWidth);
                netHeader.clientScreenHeight = htonl(header.clientScreenHeight);

                ssize_t sent = send(spectator_sock, &netHeader, sizeof(netHeader), 0);
                if (sent != sizeof(netHeader)) {
                    std::cerr << "Failed to send header to spectator" << std::endl;
                    close(spectator_sock);
                    spectator_sock = -1;
                    goto cleanup;
                }
                sent = send(spectator_sock, pixels.data(), pixels.size(), 0);
                if (sent != static_cast<ssize_t>(pixels.size())) {
                    std::cerr << "Failed to send pixel data to spectator" << std::endl;
                    close(spectator_sock);
                    spectator_sock = -1;
                    goto cleanup;
                }
                
                // Отладочная информация об отправке
                if (frameCount % 100 == 0) {
                    std::cout << "Sent frame to spectator" << std::endl;
                }
            }
        }
    }

cleanup:
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        global_client_sock = -1;
    }
    close(client_sock);
    std::cout << "Client " << client_sock << " removed." << std::endl;
}

void handle_client(int client_sock) {
    // Сначала определяем тип клиента
    char clientType[12];
    int bytes = recv(client_sock, clientType, 12, MSG_WAITALL);
    if (bytes <= 0) {
        std::cout << "Client " << client_sock << " disconnected during handshake" << std::endl;
        close(client_sock);
        return;
    }
    
    // Проверяем маркер spectator
    uint32_t marker, width, height;
    memcpy(&marker, clientType, 4);
    memcpy(&width, clientType + 4, 4);
    memcpy(&height, clientType + 8, 4);
    marker = ntohl(marker);
    width = ntohl(width);
    height = ntohl(height);
    
    std::cout << "Received handshake - marker: 0x" << std::hex << marker << std::dec 
              << ", width: " << width << ", height: " << height << std::endl;
    
    // Если это маркер spectator
    if (marker == 0x53504543) { // "SPEC"
        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            if (spectator_sock != -1) {
                std::cout << "Spectator already connected, rejecting new spectator" << std::endl;
                close(client_sock);
                return;
            }
            spectator_sock = client_sock;
            spectatorWidth = width;
            spectatorHeight = height;
        }
        std::cout << "Spectator connected with resolution: " << width << "x" << height << std::endl;
        
        // Обрабатываем входящие кадры от spectator (если будут)
        handle_spectator(client_sock);
    } else {
        // Это обычный клиент
        std::cout << "Regular client connecting..." << std::endl;
        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            if (global_client_sock != -1) {
                std::cout << "Client already connected, rejecting new client" << std::endl;
                close(client_sock);
                return;
            }
            global_client_sock = client_sock;
        }
        
        // Отправляем информацию о разрешении spectator
        uint32_t netWidth = htonl(spectatorWidth);
        uint32_t netHeight = htonl(spectatorHeight);
        std::cout << "Sending spectator resolution to client: " << spectatorWidth << "x" << spectatorHeight << std::endl;
        send(client_sock, &netWidth, 4, 0);
        send(client_sock, &netHeight, 4, 0);
        
        std::cout << "New client connected." << std::endl;
        
        // Обрабатываем входящие кадры от клиента
        handle_screen_client(client_sock);
    }
}

int main() {
    // Устанавливаем обработчик сигналов
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "Socket creation failed" << std::endl;
        return 1;
    }

    sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    addr.sin_addr.s_addr = INADDR_ANY;

    // Устанавливаем опцию для переиспользования адреса
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Bind failed" << std::endl;
        close(sock);
        return 1;
    }

    if (listen(sock, 5) < 0) {
        std::cerr << "Listen failed" << std::endl;
        close(sock);
        return 1;
    }

    std::cout << "Server started on port 8080. Waiting for connections..." << std::endl;

    while (running) {
        int new_sock = accept(sock, nullptr, nullptr);
        if (new_sock < 0) {
            std::cerr << "Accept failed" << std::endl;
            continue;
        }

        // Запускаем отдельный поток для обработки клиента
        std::thread(handle_client, new_sock).detach();
    }

    // Закрываем все клиентские соединения
    std::cout << "Closing all client connections..." << std::endl;
    {
        std::lock_guard<std::mutex> lock(clients_mutex);
        if (global_client_sock != -1) {
            close(global_client_sock);
        }
        if (spectator_sock != -1) {
            close(spectator_sock);
        }
    }

    close(sock);
    std::cout << "Server shutdown complete." << std::endl;
    return 0;
}