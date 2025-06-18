#include <iostream>
#include <vector>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <algorithm>

std::vector<int> clients;  // Используем int для файловых дескрипторов в Linux

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
            std::cout << "Client disconnected" << std::endl;
            break;
        }

        // Проверяем магическое число и корректность размеров
        header.magic = ntohl(header.magic);
        header.width = ntohl(header.width);
        header.height = ntohl(header.height);
        header.dataSize = ntohl(header.dataSize);

        if (header.magic != 0x12345678 ||
            header.width > 16384 ||
            header.height > 16384 ||
            header.dataSize != header.width * header.height * 4) {
            std::cerr << "Invalid frame header" << std::endl;
            break;
        }

        // Получаем данные изображения
        std::vector<char> pixels(header.dataSize);
        size_t totalReceived = 0;
        while (totalReceived < header.dataSize) {
            bytes = recv(client_sock, pixels.data() + totalReceived,
                header.dataSize - totalReceived, 0);
            if (bytes <= 0) break;
            totalReceived += bytes;
        }

        // Пересылаем всем другим клиентам
        for (int other_sock : clients) {
            if (other_sock != client_sock) {
                send(other_sock, &header, sizeof(header), 0);
                send(other_sock, pixels.data(), pixels.size(), 0);
            }
        }
    }

    clients.erase(std::remove(clients.begin(), clients.end(), client_sock), clients.end());
    close(client_sock);
}

int main() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "Socket creation failed" << std::endl;
        return 1;
    }

    sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    addr.sin_addr.s_addr = INADDR_ANY;

    // Устанавливаем опцию для повторного использования адреса
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

    while (true) {
        int client_sock = accept(sock, nullptr, nullptr);
        if (client_sock < 0) {
            std::cerr << "Accept failed" << std::endl;
            continue;
        }

        // Запускаем новый поток для каждого клиента
        std::thread(handle_client, client_sock).detach();
    }

    close(sock);
    return 0;
}