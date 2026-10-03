#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <filesystem>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

namespace fs = std::filesystem;

const int PORT = 8080; // Порт сервера для приема тасков (заменить на свой)

const int BLOCK_SIZE = 4096; // Размер блока для чтения

std::string getCurrentDateTimeString() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d_%H-%M-%S");
    return ss.str();
}

void saveBinaryFile(const fs::path& path, const std::string& data) {
    std::ofstream ofs(path, std::ios::binary);
    if (ofs.is_open()) {
        ofs.write(data.data(), data.size());
        ofs.close();
    }
}

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "Ошибка создания сокета" << std::endl;
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY; 
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "Ошибка bind: порт " << PORT << " занят" << std::endl;
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 10) < 0) {
        std::cerr << "Ошибка listen" << std::endl;
        close(server_fd);
        return 1;
    }

    if (!fs::exists("./tasks")) {
        fs::create_directory("./tasks");
    }

    std::cout << "=== Сервер запущен на порту " << PORT << " ===" << std::endl;

    while (true) {
        socklen_t addrlen = sizeof(address);
        int client_fd = accept(server_fd, (struct sockaddr*)&address, &addrlen);
        
        if (client_fd < 0) {
            std::cerr << "Ошибка accept" << std::endl;
            continue;
        }

        std::string full_request = "";
        char read_buf[BLOCK_SIZE];
        ssize_t bytes_read = 0;
        size_t content_length = 0;
        size_t header_end_pos = std::string::npos;

        // Читаем данные в цикле, пока не скачаем весь Content-Length
        while (true) {
            bytes_read = read(client_fd, read_buf, BLOCK_SIZE);
            if (bytes_read <= 0) break; // Клиент закрыл соединение или ошибка

            full_request.append(read_buf, bytes_read);

            // Если заголовки еще не полностью прочитаны, ищем их конец
            if (header_end_pos == std::string::npos) {
                header_end_pos = full_request.find("\r\n\r\n");
                if (header_end_pos != std::string::npos) {
                    // Вытаскиваем Content-Length, чтобы знать точный размер тела
                    size_t cl_pos = full_request.find("Content-Length:");
                    if (cl_pos != std::string::npos && cl_pos < header_end_pos) {
                        size_t cl_end = full_request.find("\r\n", cl_pos);
                        std::string cl_str = full_request.substr(cl_pos + 15, cl_end - (cl_pos + 15));
                        // Убираем пробелы
                        cl_str.erase(0, cl_str.find_first_not_of(" \t"));
                        content_length = std::stoull(cl_str);
                    }
                }
            }

            // Условие выхода: если заголовки найдены и мы дочитали все тело запроса
            if (header_end_pos != std::string::npos) {
                size_t current_body_size = full_request.size() - (header_end_pos + 4);
                if (current_body_size >= content_length) {
                    break; // Все данные получены!
                }
            }
        }

        if (full_request.empty()) {
            close(client_fd);
            continue;
        }

        // Игнорируем всё, что не является POST multipart/form-data
        size_t boundary_pos = full_request.find("boundary=");
        if (full_request.find("POST") != 0 || boundary_pos == std::string::npos || header_end_pos == std::string::npos) {
            std::cout << "[ИГНОР] Невалидный запрос (не форма)" << std::endl;
            std::string http_response = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
            write(client_fd, http_response.c_str(), http_response.length());
            close(client_fd);
            continue;
        }

        // Получаем строку boundary
        size_t rn_pos = full_request.find("\r\n", boundary_pos);
        std::string boundary = "--" + full_request.substr(boundary_pos + 9, rn_pos - (boundary_pos + 9));

        // Вырезаем только тело запроса
        std::string body = full_request.substr(header_end_pos + 4);

        std::string comment = "";
        fs::path targetDir = "./tasks/" + getCurrentDateTimeString();
        bool dirCreated = false;

        size_t pos = 0;
        while ((pos = body.find(boundary, pos)) != std::string::npos) {
            pos += boundary.length();
            if (pos + 2 <= body.size() && body.substr(pos, 2) == "--") break; // Конец всей формы
            if (pos + 2 <= body.size() && body.substr(pos, 2) == "\r\n") pos += 2;

            size_t header_end = body.find("\r\n\r\n", pos);
            if (header_end == std::string::npos) break;

            std::string part_headers = body.substr(pos, header_end - pos);
            size_t val_start = header_end + 4;
            
            size_t val_end = body.find("\r\n" + boundary, val_start);
            if (val_end == std::string::npos) val_end = body.find(boundary, val_start);
            if (val_end == std::string::npos) break;

            std::string part_content = body.substr(val_start, val_end - val_start);

            if (part_headers.find("name=\"comment\"") != std::string::npos) {
                comment = part_content;
            } 
            else if (part_headers.find("filename=\"") != std::string::npos) {
                size_t fn_start = part_headers.find("filename=\"") + 10;
                size_t fn_end = part_headers.find("\"", fn_start);
                std::string filename = part_headers.substr(fn_start, fn_end - fn_start);

                if (!filename.empty()) {
                    if (!dirCreated) {
                        fs::create_directories(targetDir);
                        dirCreated = true;
                    }
                    fs::path safe_filename = fs::path(filename).filename();
                    saveBinaryFile(targetDir / safe_filename, part_content);
                    std::cout << "[Файл]: Успешно сохранен в " << targetDir.string() << "/" << safe_filename.string() << " (" << part_content.size() << " байт)" << std::endl;
                }
            }
            pos = val_end;
        }

        std::cout << "\n[Новая заявка!]:" << std::endl;
	
	std::ofstream description(targetDir.string() + "/text.txt");

	if (!description.is_open()) {
        	std::cerr << "Не удалось открыть файл (коммент в консоли)" << std::endl;
    	}
	
	description << "Комментарий: " << comment << "\n";

	description.close();

        std::cout << "Комментарий: " << comment << std::endl;
        std::cout << "--------------------------------------" << std::endl;

        std::string res_body = "Данные формы приняты сервером!";
        std::string http_response = 
            "HTTP/1.1 200 OK\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Content-Length: " + std::to_string(res_body.length()) + "\r\n"
            "Connection: close\r\n\r\n" + res_body;

        write(client_fd, http_response.c_str(), http_response.length());
        close(client_fd);
    }

    close(server_fd);
    return 0;
}
