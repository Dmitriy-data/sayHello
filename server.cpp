#include <iostream>
#include <vector>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <algorithm>
#include <signal.h>

std::vector<int> clients;  // Используем int для дескрипторов сокетов в Linux
volatile bool running = true;

void signal_handler(int sig) {
    std::cout << "\nReceived signal " << sig << ". Shutting down server..." << std::endl;
    running = false;
}

void handle_client(int client_sock) {
    clients.push_back(client_sock);
    std::cout << "New client connected. Total clients: " << clients.size() << std::endl;

#pragma pack(push, 1)
    struct FrameHeader {
        uint32_t magic;
        uint32_t width;
        uint32_t height;
        uint32_t dataSize;
    };
#pragma pack(pop)

    char buffer[65536];

    while (true) {
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

        if (header.magic != 0x12345678 ||
            header.width > 16384 ||
            header.height > 16384 ||
            header.dataSize != header.width * header.height * 4) {
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

        // Отправляем данные всем клиентам
        for (int other_sock : clients) {
            if (other_sock != client_sock) {
                // Отправляем заголовок
                ssize_t sent = send(other_sock, &header, sizeof(header), 0);
                if (sent != sizeof(header)) {
                    std::cerr << "Failed to send header to client " << other_sock << std::endl;
                    continue;
                }
                
                // Отправляем данные пикселей
                sent = send(other_sock, pixels.data(), pixels.size(), 0);
                if (sent != static_cast<ssize_t>(pixels.size())) {
                    std::cerr << "Failed to send pixel data to client " << other_sock << std::endl;
                    continue;
                }
            }
        }
    }

cleanup:
    clients.erase(std::remove(clients.begin(), clients.end(), client_sock), clients.end());
    close(client_sock);
    std::cout << "Client " << client_sock << " removed. Total clients: " << clients.size() << std::endl;
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

    // ������������� ����� ��� ���������� ������������� ������
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
        int client_sock = accept(sock, nullptr, nullptr);
        if (client_sock < 0) {
            std::cerr << "Accept failed" << std::endl;
            continue;
        }

        // ��������� ����� ����� ��� ������� �������
        std::thread(handle_client, client_sock).detach();
    }

    // Закрываем все клиентские соединения
    std::cout << "Closing all client connections..." << std::endl;
    for (int client_sock : clients) {
        close(client_sock);
    }
    clients.clear();

    close(sock);
    std::cout << "Server shutdown complete." << std::endl;
    return 0;
}