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

    char buffer[65536];
    while (true) {
        int bytes = recv(client_sock, buffer, sizeof(buffer), 0);
        if (bytes <= 0) {
            std::cout << "Client disconnected" << std::endl;
            break;
        }

        for (int other_sock : clients) {
            if (other_sock != client_sock) {
                send(other_sock, buffer, bytes, 0);
            }
        }
    }

    // Удаляем клиента из списка (исправленная версия)
    clients.erase(
        std::remove(clients.begin(), clients.end(), client_sock),
        clients.end()
    );
    close(client_sock);  // В Linux используем close() вместо closesocket()
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