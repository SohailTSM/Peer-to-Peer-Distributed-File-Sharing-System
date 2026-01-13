#include <iostream>
#include <string.h>
#include <string>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <vector>
#include <utility>
#include <arpa/inet.h>
#include <netdb.h>
#include <atomic>
#include <csignal>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <thread>
#include <mutex>
#define CHUNK_SIZE (512 * 1024) // 512KB

using namespace std;

vector<pair<string, string>> completed_downloads;
mutex completed_downloads_mutex;


void error(const char *msg);
vector<pair<string, string>> extractTrackerInfo(char *argv[]);
string computeFileSHA1(const string &file_path);
string computeChunkHashes(const string &file_path, string &chunk_hashes_out);
void notifyNewSeeder(int socketFD, const string &group_id, const string &file_name, const string &file_path, int my_port);
int getMyPort(int argc, char *argv[]);
void runDownloadListener(int my_port, string my_user_id);

int main(int argc, char *argv[])
{
    int serverFD, portno, n, opt = 1, socketFD;
    struct sockaddr_in serverAddress;
    struct hostent *server;

    char buffer[307200];

    if (argc < 3)
    {
        cout << "Usage : ./client <IP>:<PORT_NO> <tracker_info_file>\n";
        exit(0);
    }

    pair<string, string> ipPort({"", ""});
    int flag = 0;
    for (auto s : (string)argv[1])
    {
        if (s == ':')
        {
            flag++;
            continue;
        }
        if (!flag)
            ipPort.first += s;
        else
            ipPort.second += s;
    }

    int my_port = getMyPort(argc, argv);
    string my_user_id = "";
    std::thread dl_thread(runDownloadListener, my_port, my_user_id);
    dl_thread.detach();

    vector<pair<string, string>> trackers = extractTrackerInfo(argv);

    server = gethostbyname(ipPort.first.c_str());
    if (server == NULL)
    {
        fprintf(stderr, "ERROR, no such host\n");
        exit(0);
    }

    bzero((char *)&serverAddress, sizeof(serverAddress));
    serverAddress.sin_family = AF_INET;
    bcopy((char *)server->h_addr,
          (char *)&serverAddress.sin_addr.s_addr,
          server->h_length);

    while (true)
    {
        int counter = 1;
        for (auto track : trackers)
        {
            socketFD = socket(AF_INET, SOCK_STREAM, 0);
            if (socketFD < 0)
                error("ERROR opening socket");
            portno = stoi(track.second);
            serverAddress.sin_port = htons(portno);
            cout << "Trying to connect to tracker on port : " << portno << endl
                 << flush;
            if (connect(socketFD, (struct sockaddr *)&serverAddress, sizeof(serverAddress)) < 0)
            {
                if (counter >= 2)
                {
                    cout << "Connection can not be established to port : " << portno << endl
                         << flush;
                    cout << "No more trackers available exiting the client";
                    close(socketFD);
                    exit(0);
                }
                else
                {
                    counter++;
                    cout << "Connection can not be established to port : " << portno << endl
                         << flush;
                }
            }
            else
            {
                cout << "Connected to tracker on port : " << portno << endl
                     << flush;
                break;
            }
        }

        while (true)
        {
            cout << "$> " << flush;
            bzero(buffer, sizeof(buffer));
            cin.getline(buffer, sizeof(buffer));
            if ((string)buffer == "exit")
                break;

            // Handle upload_file command
            char cmd[20], group_id[128], file_path[512], dest_path[512], file_name[512];
            if (sscanf(buffer, "%s %s %s", cmd, group_id, file_path) == 3 && strcmp(cmd, "upload_file") == 0)
            {
                int fd = open(file_path, O_RDONLY);
                if (fd < 0)
                {
                    cout << "Error: Cannot open file." << endl
                         << flush;
                    continue;
                }
                off_t file_size = lseek(fd, 0, SEEK_END);
                close(fd);

                string file_hash = computeFileSHA1(file_path);
                if (file_hash.empty())
                {
                    cout << "Error: Cannot open file." << endl
                         << flush;
                    continue;
                }
                string chunk_hashes;
                computeChunkHashes(file_path, chunk_hashes);

                char upload_cmd[307200];
                snprintf(upload_cmd, sizeof(upload_cmd), "upload_file %s %s %ld %s %s %d\n", group_id, file_path, file_size, file_hash.c_str(), chunk_hashes.c_str(), my_port);
                n = write(socketFD, upload_cmd, strlen(upload_cmd));
            }
            else if (sscanf(buffer, "%s %s %s %s", cmd, group_id, file_name, dest_path) == 4 && strcmp(cmd, "download_file") == 0)
            {
                // Send download_file request to tracker
                char download_cmd[307200];
                snprintf(download_cmd, sizeof(download_cmd), "download_file %s %s %s\n", group_id, file_name, dest_path);
                n = write(socketFD, download_cmd, strlen(download_cmd));
                if (n < 0)
                    error("ERROR writing to socket");
                bzero(buffer, sizeof(buffer));
                n = read(socketFD, buffer, sizeof(buffer) - 1);
                if (n <= 0)
                    error("ERROR reading from socket");

                // Parse tracker response
                string response(buffer);
                string file_name, file_hash, file_size_str, file_path, chunk_hashes_str, seeders_str, seeders_ports_str;
                size_t pos = 0;
                auto get_line = [&](string &s)
                {
                    size_t p = s.find('\n');
                    string line = s.substr(0, p);
                    s = (p == string::npos) ? "" : s.substr(p + 1);
                    return line;
                };
                auto safe_substr = [](const string &line, const char *prefix)
                {
                    size_t prefix_len = strlen(prefix);
                    if (line.size() >= prefix_len)
                        return line.substr(prefix_len);
                    return string("");
                };
                file_name = safe_substr(get_line(response), "file_name:");
                file_size_str = safe_substr(get_line(response), "file_size:");
                file_hash = safe_substr(get_line(response), "file_hash:");
                chunk_hashes_str = safe_substr(get_line(response), "chunk_hashes:");
                seeders_str = safe_substr(get_line(response), "seeders:");
                seeders_ports_str = safe_substr(get_line(response), "seeder_ports:");
                string seeder_file_paths_str = safe_substr(get_line(response), "seeder_file_paths:");

                cout << "File: " << file_name << endl;
                cout << "Size: " << file_size_str << endl;
                cout << "Hash: " << file_hash << endl;
                cout << "Chunks: " << chunk_hashes_str << endl;
                cout << "Seeders: " << seeders_str << endl;
                cout << "Seeder Ports: " << seeders_ports_str << endl;
                cout << "Seeder File Paths: " << seeder_file_paths_str << endl;

                // Parse seeders and ports
                vector<string> seeders;
                if (seeders_str != "None")
                {
                    size_t start = 0, end;
                    while ((end = seeders_str.find(',', start)) != string::npos)
                    {
                        seeders.push_back(seeders_str.substr(start, end - start));
                        start = end + 1;
                    }
                    if (start < seeders_str.size())
                        seeders.push_back(seeders_str.substr(start));
                }
                vector<int> seeder_ports;
                if (seeders_ports_str != "None")
                {
                    size_t start = 0, end;
                    while ((end = seeders_ports_str.find(',', start)) != string::npos)
                    {
                        seeder_ports.push_back(stoi(seeders_ports_str.substr(start, end - start)));
                        start = end + 1;
                    }
                    if (start < seeders_ports_str.size())
                        seeder_ports.push_back(stoi(seeders_ports_str.substr(start)));
                }
                vector<string> seeder_file_paths;
                if (seeder_file_paths_str != "None")
                {
                    size_t start = 0, end;
                    while ((end = seeder_file_paths_str.find(',', start)) != string::npos)
                    {
                        seeder_file_paths.push_back(seeder_file_paths_str.substr(start, end - start));
                        start = end + 1;
                    }
                    if (start < seeder_file_paths_str.size())
                        seeder_file_paths.push_back(seeder_file_paths_str.substr(start));
                }
                vector<string> chunk_hashes;
                size_t start = 0, end;
                while ((end = chunk_hashes_str.find(',', start)) != string::npos)
                {
                    chunk_hashes.push_back(chunk_hashes_str.substr(start, end - start));
                    start = end + 1;
                }
                if (start < chunk_hashes_str.size())
                    chunk_hashes.push_back(chunk_hashes_str.substr(start));

                // Download chunks from seeders
                int outfd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                if (outfd < 0)
                {
                    cout << "Error: Cannot open destination file." << endl;
                    continue;
                }
                int num_chunks = chunk_hashes.size();
                const int THREAD_POOL_SIZE = 8;
                vector<vector<char>> chunk_datas(num_chunks);
                vector<bool> chunk_ok(num_chunks, false);
                mutex write_mutex;
                auto download_chunk = [&](int i)
                {
                    int seeder_idx = i % seeders.size();
                    string seeder = seeders[seeder_idx];
                    int seeder_port = seeder_ports[seeder_idx];
                    string seeder_path = (seeder_file_paths.size() > seeder_idx) ? seeder_file_paths[seeder_idx] : "unknown";
                    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
                    if (sockfd < 0)
                    {
                        cout << "Error: Cannot create socket to seeder." << endl;
                        return;
                    }
                    struct sockaddr_in seeder_addr;
                    seeder_addr.sin_family = AF_INET;
                    seeder_addr.sin_port = htons(seeder_port);
                    seeder_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
                    if (connect(sockfd, (struct sockaddr *)&seeder_addr, sizeof(seeder_addr)) < 0)
                    {
                        cout << "Raw tracker response:\n"
                             << response << endl;
                        cout << "Error: Cannot connect to seeder " << seeder << " on port " << seeder_port << endl;
                        close(sockfd);
                        return;
                    }
                    char chunk_req[1024];
                    snprintf(chunk_req, sizeof(chunk_req), "GET_CHUNK %d %s\n", i, seeder_path.c_str());
                    write(sockfd, chunk_req, strlen(chunk_req));
                    int expected_chunk_size = CHUNK_SIZE;
                    if ((i + 1) == num_chunks)
                    {
                        long total_size = file_size_str.empty() ? 0 : stol(file_size_str);
                        long last_offset = i * CHUNK_SIZE;
                        if (total_size > last_offset)
                            expected_chunk_size = total_size - last_offset;
                    }
                    vector<char> chunk_buf(expected_chunk_size);
                    int bytes_read = 0;
                    while (bytes_read < expected_chunk_size)
                    {
                        int n = read(sockfd, chunk_buf.data() + bytes_read, expected_chunk_size - bytes_read);
                        if (n <= 0)
                            break;
                        bytes_read += n;
                    }
                    chunk_buf.resize(bytes_read);
                    // Hash check
                    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
                    EVP_DigestInit_ex(mdctx, EVP_sha1(), NULL);
                    EVP_DigestUpdate(mdctx, chunk_buf.data(), chunk_buf.size());
                    unsigned char hash[EVP_MAX_MD_SIZE];
                    unsigned int hash_len;
                    EVP_DigestFinal_ex(mdctx, hash, &hash_len);
                    EVP_MD_CTX_free(mdctx);
                    char hex_hash[hash_len * 2 + 1];
                    for (unsigned int k = 0; k < hash_len; ++k)
                        sprintf(hex_hash + k * 2, "%02x", hash[k]);
                    hex_hash[hash_len * 2] = '\0';
                    if (string(hex_hash) != chunk_hashes[i])
                    {
                        cout << "Error: Chunk hash mismatch for chunk " << i << endl;
                        close(sockfd);
                        return;
                    }
                    {
                        lock_guard<mutex> lock(write_mutex);
                        chunk_datas[i] = std::move(chunk_buf);
                        chunk_ok[i] = true;
                    }
                    cout << "Chunk " << i << " downloaded from seeder " << seeder << " (port " << seeder_port << ", path " << seeder_path << ")" << endl;
                    close(sockfd);
                };
                vector<thread> threads;
                atomic<int> next_chunk(0);
                auto worker = [&]()
                {
                    while (true)
                    {
                        int i = next_chunk++;
                        if (i >= num_chunks)
                            break;
                        download_chunk(i);
                    }
                };
                for (int t = 0; t < THREAD_POOL_SIZE; ++t)
                {
                    threads.emplace_back(worker);
                }
                for (auto &th : threads)
                    th.join();
                // Write all chunks to file in order
                for (int i = 0; i < num_chunks; ++i)
                {
                    if (!chunk_ok[i])
                    {
                        cout << "Error: Missing chunk " << i << ". Download failed." << endl;
                        close(outfd);
                        remove(dest_path);
                        goto download_end;
                    }
                    if (write(outfd, chunk_datas[i].data(), chunk_datas[i].size()) != (int)chunk_datas[i].size())
                    {
                        cout << "Error: Failed to write chunk " << i << " to file." << endl;
                        close(outfd);
                        remove(dest_path);
                        goto download_end;
                    }
                }
                close(outfd);
                cout << "Download complete. File saved to " << dest_path << endl;
                // Track completed download
                {
                    lock_guard<mutex> lock(completed_downloads_mutex);
                    completed_downloads.push_back({group_id, file_name});
                }
                // After download, become a seeder for this file
                notifyNewSeeder(socketFD, group_id, file_name, dest_path, my_port);
            download_end:;
            }
            else if (strcmp(buffer, "show_downloads") == 0)
            {
                lock_guard<mutex> lock(completed_downloads_mutex);
                if (completed_downloads.empty())
                {
                    cout << "No completed downloads." << endl;
                }
                else
                {
                    for (const auto &entry : completed_downloads)
                    {
                        cout << "[C] " << entry.first << " " << entry.second << endl;
                    }
                }
                continue;
            }
            else
            {
                if (strlen(buffer) < 2)
                {
                    cout << "Invalid command" << endl
                         << flush;
                    continue;
                }
                // Add newline to all other commands
                strcat(buffer, "\n");
                n = write(socketFD, buffer, strlen(buffer));
            }

            if (n < 0)
                error("ERROR writing to socket");
            bzero(buffer, sizeof(buffer));
            n = read(socketFD, buffer, sizeof(buffer) - 1);
            if (n == 0)
                break;
            if (n < 0)
                error("ERROR reading from socket");
            if (n < 1)
            {
                continue;
            }
            cout << buffer << endl
                 << flush;
        }
        close(socketFD);
    }

    return 0;
}

vector<pair<string, string>> extractTrackerInfo(char *argv[])
{
    string trackerInfoFileName = string(argv[2]);

    char trackerBuffer[307200];
    int trackerInfoFD = open(trackerInfoFileName.c_str(), O_RDONLY);
    ssize_t readBytes = read(trackerInfoFD, trackerBuffer, sizeof(trackerBuffer));

    vector<string> trackerAddressList(2, "");
    int trackerPos = 0;
    for (int i = 0; i < readBytes; i++)
    {
        if (trackerBuffer[i] == '\n')
        {
            trackerPos++;
            continue;
        }
        trackerAddressList[trackerPos] += trackerBuffer[i];
    }

    vector<pair<string, string>> trackers(2, pair<string, string>("", ""));
    trackerPos = 0;
    for (auto t : trackerAddressList)
    {

        bool first = true;
        for (auto c : t)
        {
            if (c == ':')
            {
                first = false;
                continue;
            }
            if (first)
                trackers[trackerPos].first += c;
            else
                trackers[trackerPos].second += c;
        }
        trackerPos++;
    }
    return trackers;
}

void error(const char *msg)
{
    perror(msg);
    exit(1);
}

string computeFileSHA1(const string &file_path)
{
    int fd = open(file_path.c_str(), O_RDONLY);
    if (fd < 0)
        return "";
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(mdctx, EVP_sha1(), NULL);
    char buf[8192];
    ssize_t bytes;
    while ((bytes = read(fd, buf, sizeof(buf))) > 0)
    {
        EVP_DigestUpdate(mdctx, buf, bytes);
    }
    close(fd);
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;
    EVP_DigestFinal_ex(mdctx, hash, &hash_len);
    EVP_MD_CTX_free(mdctx);
    char hex_hash[hash_len * 2 + 1];
    for (unsigned int i = 0; i < hash_len; ++i)
        sprintf(hex_hash + i * 2, "%02x", hash[i]);
    hex_hash[hash_len * 2] = '\0';
    return string(hex_hash);
}

string computeChunkHashes(const string &file_path, string &chunk_hashes_out)
{
    int fd = open(file_path.c_str(), O_RDONLY);
    if (fd < 0)
        return "";
    char buf[CHUNK_SIZE];
    ssize_t bytes;
    chunk_hashes_out = "";
    int chunk_count = 0;
    while ((bytes = read(fd, buf, sizeof(buf))) > 0)
    {
        EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(mdctx, EVP_sha1(), NULL);
        EVP_DigestUpdate(mdctx, buf, bytes);
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int hash_len;
        EVP_DigestFinal_ex(mdctx, hash, &hash_len);
        EVP_MD_CTX_free(mdctx);
        char hex_hash[hash_len * 2 + 1];
        for (unsigned int i = 0; i < hash_len; ++i)
            sprintf(hex_hash + i * 2, "%02x", hash[i]);
        hex_hash[hash_len * 2] = '\0';
        if (chunk_count > 0)
            chunk_hashes_out += ",";
        chunk_hashes_out += hex_hash;
        chunk_count++;
    }
    close(fd);
    return chunk_hashes_out;
}

// Internal command to notify tracker of new seeder after download
void notifyNewSeeder(int socketFD, const string &group_id, const string &file_name, const string &file_path, int my_port)
{
    char notify_cmd[1024];
    snprintf(notify_cmd, sizeof(notify_cmd), "new_seeder %s %s %s %d\n", group_id.c_str(), file_name.c_str(), file_path.c_str(), my_port);
    write(socketFD, notify_cmd, strlen(notify_cmd));
    cout << "Notified tracker: new seeder for " << file_name << " at path " << file_path << endl;
}

int getMyPort(int argc, char *argv[])
{
    if (argc < 2)
        return 0;
    string arg = argv[1];
    size_t pos = arg.find(":");
    if (pos == string::npos)
        return 0;
    return stoi(arg.substr(pos + 1));
}

void runDownloadListener(int my_port, string my_user_id)
{
    int serverFD = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFD < 0)
    {
        perror("Download listener socket error");
        return;
    }
    int opt = 1;
    setsockopt(serverFD, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(my_port);
    if (bind(serverFD, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("Download listener bind error");
        close(serverFD);
        return;
    }
    listen(serverFD, 5);
    cout << "Download listener running on port " << my_port << endl;
    while (true)
    {
        int clientFD = accept(serverFD, NULL, NULL);
        if (clientFD < 0)
            continue;
        std::thread([clientFD, my_user_id]()
                    {
            char req[128];
            bzero(req, sizeof(req));
            int n = read(clientFD, req, sizeof(req) - 1);
            if (n <= 0) { close(clientFD); return; }
            req[n] = '\0';
            int chunk_idx = -1;
            char file_path[512];
            if (sscanf(req, "GET_CHUNK %d %s", &chunk_idx, file_path) == 2 && chunk_idx >= 0) {

                int fd = open(file_path, O_RDONLY);
                if (fd < 0) { printf("[Download Listener] Error opening file: %s\n", file_path); close(clientFD); return; }
                off_t offset = chunk_idx * CHUNK_SIZE;
                off_t file_size = lseek(fd, 0, SEEK_END);
                lseek(fd, offset, SEEK_SET);
                size_t to_read = CHUNK_SIZE;
                if (offset + CHUNK_SIZE > file_size)
                    to_read = file_size > offset ? file_size - offset : 0;

                char buf[CHUNK_SIZE];
                int bytes = read(fd, buf, to_read);

                if (bytes > 0) write(clientFD, buf, bytes);
                close(fd);
            }
            close(clientFD); })
            .detach();
    }
}
