#include <iostream>
#include <string.h>
#include <string>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <vector>
#include <map>
#include <utility>
#include <algorithm>
#include <thread>
#include <arpa/inet.h>
#include <netdb.h>
#include <mutex>

using namespace std;

const string change_marker = "123456789";

void error(const char *msg);
vector<pair<string, string>> extractTrackerInfo(char *argv[]);
void handleCommand(char *request, char *response, int userPort);
void handleSync(char *request);
vector<string> split(char *request);
string createUser(vector<string> &command);
string login(vector<string> &command, int userPort);
string createGroup(vector<string> &command, int userPort);
string joinGroup(vector<string> &command, int userPort);
string leaveGroup(vector<string> &command, int userPort);
string listGroups(vector<string> &command, int userPort);
string listRequests(vector<string> &command, int userPort);
string acceptRequest(vector<string> &command, int userPort);
string logout(vector<string> &command, int userPort);
void syncServer(vector<string> command, int userPort);
string uploadFile(vector<string> &command, int userPort);
string listFiles(vector<string> &command, int userPort);
string downloadFile(vector<string> &command, int userPort);
string handleNewSeeder(vector<string> &command, int userPort);
string stopShare(vector<string> &command, int userPort);

class User
{
public:
    string id;
    string password;

    User(string _id, string _password)
    {
        id = _id;
        password = _password;
    }

    User() {}
};

class Group
{
public:
    string groupName;
    vector<User> members;
    User owner;
    vector<User> requests;

    Group(string _name, User _owner)
    {
        owner = _owner;
        groupName = _name;
        members.push_back(_owner);
    }

    Group() {}
};

map<string, User> users;
map<string, Group> groups;
map<string, int> isAuth;
map<int, string> portToUser;
vector<pair<string, string>> trackers;
int trackerNumber;
int otherTrackerNumber;
int synced = 0;
int doSync = 0;
vector<pair<vector<string>, int>> toSync;
mutex users_mutex;
mutex groups_mutex;
mutex isAuth_mutex;
mutex portToUser_mutex;
mutex toSync_mutex;

class FileInfo
{
public:
    string file_name;
    string file_path;
    string owner;
    string group_id;
    long file_size;
    string file_hash;
    vector<string> chunk_hashes;

    FileInfo(string fpath, string fname, string own, string gid, long fsize, string fhash, vector<string> chashes)
        : file_path(fpath), file_name(fname), owner(own), group_id(gid), file_size(fsize), file_hash(fhash), chunk_hashes(chashes) {}
    FileInfo() {}
};

map<string, vector<FileInfo>> group_files;             // group_id -> files
map<string, map<string, vector<string>>> file_seeders; // group_id -> file_name -> vector<user_id>
map<string, int> user_ports;                           // user_id -> port
map<string, map<string, string>> seeder_file_paths;    // user_id -> file_name -> file_path

int main(int argc, char *argv[])
{
    int serverFD, newFD, serverSocket;
    socklen_t clientLen;
    sockaddr_in serverAddress, clientAddress;
    int opt = 1;

    clientLen = sizeof(clientAddress);

    if (argc < 3)
    {
        cout << "Usage : ./tracker <tracker_info_file> <tracker_no>\n";
        exit(0);
    }

    trackers = extractTrackerInfo(argv);
    trackerNumber = atoi(argv[2]);

    if (trackerNumber == 1)
        otherTrackerNumber = 2;
    else
        otherTrackerNumber = 1;
    int port = stoi(trackers[trackerNumber - 1].second);

    serverFD = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFD < 0)
        error("ERROR opening socket");

    bzero((char *)&serverAddress, sizeof(serverAddress));
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = INADDR_ANY;
    serverAddress.sin_port = htons(port);

    // Forcefully attach socket to port (reuse address)
    if (setsockopt(serverFD, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)))
        error("setsockopt failed");

    if (bind(serverFD, (sockaddr *)&serverAddress, sizeof(serverAddress)) < 0)
        error("ERROR on binding");

    listen(serverFD, 5);

    while (true)
    {
        bzero((char *)&clientAddress, sizeof(clientAddress));
        if ((newFD = accept(serverFD, (struct sockaddr *)&clientAddress, (socklen_t *)&clientLen)) < 0)
            error("ERROR on accept");

        sockaddr_in tempAddress = clientAddress;
        int tempFD = newFD;
        thread t([tempFD, tempAddress]()
                 {
                    while(true) {
                        char buffer[307200];
                        bzero(buffer, sizeof(buffer));
                        int total = 0;
                        while (true) {
                            int n = read(tempFD, buffer + total, sizeof(buffer) - 1 - total);
                            if (n <= 0) break;
                            total += n;
                            // Check if newline is present in the received data
                            if (memchr(buffer, '\n', total)) break;
                            // If buffer is full but no newline, process anyway
                            if (total >= sizeof(buffer) - 1) break;
                        }
                        if (total <= 0) break;
                        buffer[total] = '\0';

                        // Remove trailing newline if present
                        char *newline = strchr(buffer, '\n');
                        if (newline) *newline = '\0';

                        char response[307200];
                        bzero(response, sizeof(response));
                        int userPort = (int)ntohs(tempAddress.sin_port);

                        if(strncmp(buffer, change_marker.c_str(), change_marker.size()) == 0){
                            handleSync(buffer);
                            break;
                        }

                        handleCommand(buffer, response, userPort);
                        int n = write(tempFD, response, strlen(response));
                        if (n < 0)
                            error("ERROR writing to socket");
             } });

        t.detach();
    }
    close(serverFD);
    return 0;
}

void error(const char *msg)
{
    perror(msg);
    exit(1);
}

vector<pair<string, string>> extractTrackerInfo(char *argv[])
{
    string trackerInfoFileName = string(argv[1]);

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

void handleCommand(char *request, char *response, int userPort)
{
    vector<string> command = split(request);
    string res;
    if (command[0] == "create_user")
        res = createUser(command);
    else if (command[0] == "login")
        res = login(command, userPort);
    else if (command[0] == "create_group")
        res = createGroup(command, userPort);
    else if (command[0] == "join_group")
        res = joinGroup(command, userPort);
    else if (command[0] == "leave_group")
        res = leaveGroup(command, userPort);
    else if (command[0] == "list_groups")
        res = listGroups(command, userPort);
    else if (command[0] == "list_requests")
        res = listRequests(command, userPort);
    else if (command[0] == "accept_request")
        res = acceptRequest(command, userPort);
    else if (command[0] == "logout")
        res = logout(command, userPort);
    else if (command[0] == "upload_file")
        res = uploadFile(command, userPort);
    else if (command[0] == "new_seeder")
        res = handleNewSeeder(command, userPort);
    else if (command[0] == "list_files")
        res = listFiles(command, userPort);
    else if (command[0] == "download_file")
        res = downloadFile(command, userPort);
    else if (command[0] == "stop_share")
        res = stopShare(command, userPort);
    else
        res = "Not a valid command";

    if (res.size() >= change_marker.size() &&
        res.substr(res.size() - change_marker.size()) == change_marker)
    {
        res.erase(res.size() - change_marker.size());
        syncServer(command, userPort);
    }
    strcpy(response, res.c_str());
    return;
}

void handleSync(char *request)
{
    vector<string> command = split(request);
    // Remove marker from the first command string
    if (command[0].substr(0, change_marker.size()) == change_marker)
        command[0] = command[0].substr(change_marker.size());
    int userPort = stoi(command[command.size() - 1]);
    command.pop_back();
    string res;
    if (command[0] == "create_user")
        res = createUser(command);
    else if (command[0] == "login")
        res = login(command, userPort);
    else if (command[0] == "create_group")
        res = createGroup(command, userPort);
    else if (command[0] == "join_group")
        res = joinGroup(command, userPort);
    else if (command[0] == "leave_group")
        res = leaveGroup(command, userPort);
    else if (command[0] == "list_groups")
        res = listGroups(command, userPort);
    else if (command[0] == "list_requests")
        res = listRequests(command, userPort);
    else if (command[0] == "accept_request")
        res = acceptRequest(command, userPort);
    else if (command[0] == "logout")
        res = logout(command, userPort);
    else if (command[0] == "upload_file")
        res = uploadFile(command, userPort);
    else if (command[0] == "new_seeder")
        res = handleNewSeeder(command, userPort);
    else if (command[0] == "list_files")
        res = listFiles(command, userPort);
    else if (command[0] == "download_file")
        res = downloadFile(command, userPort);
    else if (command[0] == "stop_share")
        res = stopShare(command, userPort);
    else
        res = "Not a valid command";
    return;
}

vector<string> split(char *request)
{
    char command[307200];
    strcpy(command, request);
    char *saveptr, *token;
    vector<string> res;
    token = strtok_r(command, " ", &saveptr);
    while (token != NULL)
    {
        res.push_back((string)token);
        token = strtok_r(NULL, " ", &saveptr);
    }
    return res;
}

string createUser(vector<string> &command)
{
    lock_guard<mutex> lock(users_mutex);
    if (command.size() != 3)
        return "Usage : create_user <user_id> <password>";
    if (users.find(command[1]) != users.end())
        return "User already exists";
    User user(command[1], command[2]);
    users[command[1]] = user;
    return "User created successfully" + change_marker;
}

string login(vector<string> &command, int userPort)
{
    lock_guard<mutex> lock1(users_mutex);
    lock_guard<mutex> lock2(isAuth_mutex);
    lock_guard<mutex> lock3(portToUser_mutex);
    if (command.size() != 3)
        return "Usage : login <user_id> <password>";
    if (isAuth[portToUser[userPort]])
        return "Already logged in, please logout first to login again";
    if (users.find(command[1]) == users.end())
        return "User does not exist";
    if (users[command[1]].password != command[2])
        return "Invalid credentials";
    isAuth[command[1]] = 1;
    portToUser[userPort] = command[1];
    return "Login successfull" + change_marker;
}

string createGroup(vector<string> &command, int userPort)
{
    lock_guard<mutex> lock1(groups_mutex);
    lock_guard<mutex> lock2(portToUser_mutex);
    lock_guard<mutex> lock3(users_mutex);
    if (command.size() != 2)
        return "Usage : create_group <group_id>";
    if (portToUser.find(userPort) == portToUser.end() || !isAuth[portToUser[userPort]])
        return "You are not authorized, please login first";
    if (groups.find(command[1]) != groups.end())
        return "Group already exists";
    Group group(command[1], users[portToUser[userPort]]);
    groups[command[1]] = group;
    return "Group created successfully" + change_marker;
}

string joinGroup(vector<string> &command, int userPort)
{
    lock_guard<mutex> lock1(groups_mutex);
    lock_guard<mutex> lock2(portToUser_mutex);
    lock_guard<mutex> lock3(users_mutex);
    if (command.size() != 2)
        return "Usage : join_group <group_id>";
    if (portToUser.find(userPort) == portToUser.end() || !isAuth[portToUser[userPort]])
        return "You are not authorized, please login first";
    if (groups.find(command[1]) == groups.end())
        return "Group does not exists";
    if (groups[command[1]].owner.id == portToUser[userPort])
        return "You are already the owner of group";
    if (find_if(groups[command[1]].requests.begin(), groups[command[1]].requests.end(), [&](const User &u)
                { return u.id == portToUser[userPort]; }) != groups[command[1]].requests.end())
        return "You have already requested to join this group";
    if (find_if(groups[command[1]].members.begin(), groups[command[1]].members.end(), [&](const User &u)
                { return u.id == portToUser[userPort]; }) != groups[command[1]].members.end())
        return "You are already a member of this group";
    groups[command[1]].requests.push_back(users[portToUser[userPort]]);
    return "Requested to join the group, wait for owner to accept the request" + change_marker;
}

string leaveGroup(vector<string> &command, int userPort)
{
    lock_guard<mutex> lock1(groups_mutex);
    lock_guard<mutex> lock2(portToUser_mutex);
    lock_guard<mutex> lock3(users_mutex);
    if (command.size() != 2)
        return "Usage : leave_group <group_id>";
    if (portToUser.find(userPort) == portToUser.end() || !isAuth[portToUser[userPort]])
        return "You are not authorized, please login first";
    if (groups.find(command[1]) == groups.end())
        return "Group does not exists";
    if (find_if(groups[command[1]].requests.begin(), groups[command[1]].requests.end(), [&](const User &u)
                { return u.id == portToUser[userPort]; }) != groups[command[1]].requests.end())
    {
        groups[command[1]].requests.erase(find_if(groups[command[1]].requests.begin(), groups[command[1]].requests.end(), [&](const User &u)
                                                  { return u.id == portToUser[userPort]; }));
        return "You had requested to join this group, but it was not accepted. Your request has been cancelled.";
    }
    if (find_if(groups[command[1]].members.begin(), groups[command[1]].members.end(), [&](const User &u)
                { return u.id == portToUser[userPort]; }) != groups[command[1]].members.end())
    {
        groups[command[1]].members.erase(find_if(groups[command[1]].members.begin(), groups[command[1]].members.end(), [&](const User &u)
                                                 { return u.id == portToUser[userPort]; }));
        if (groups[command[1]].members.size() == 0)
            groups.erase(command[1]);
        else if (groups[command[1]].owner.id == portToUser[userPort])
            groups[command[1]].owner = groups[command[1]].members[0];
        return "Group left successfully" + change_marker;
    }
    return "You are not the member of this group";
}

string listGroups(vector<string> &command, int userPort)
{
    lock_guard<mutex> lock(groups_mutex);
    if (command.size() != 1)
        return "Usage : list_groups";
    if (portToUser.find(userPort) == portToUser.end() || !isAuth[portToUser[userPort]])
        return "You are not authorized, please login first";
    string result;
    for (auto g : groups)
    {
        result += g.first + '\n';
    }
    if (!result.empty())
        result.pop_back();
    if (result.empty())
        return "No existing groups";
    return result;
}

string listRequests(vector<string> &command, int userPort)
{
    lock_guard<mutex> lock1(groups_mutex);
    lock_guard<mutex> lock2(portToUser_mutex);
    if (command.size() != 2)
        return "Usage : list_requests <group_id>";
    if (portToUser.find(userPort) == portToUser.end() || !isAuth[portToUser[userPort]])
        return "You are not authorized, please login first";
    if (groups.find(command[1]) == groups.end())
        return "Group does not exist";
    if (groups[command[1]].owner.id != portToUser[userPort])
        return "You are not the owner of this group";
    if (!groups[command[1]].requests.size())
        return "No pending requests";
    string result;
    for (auto r : groups[command[1]].requests)
    {
        result += r.id + '\n';
    }
    if (!result.empty())
        result.pop_back();
    return result;
}

string acceptRequest(vector<string> &command, int userPort)
{
    lock_guard<mutex> lock1(groups_mutex);
    lock_guard<mutex> lock2(portToUser_mutex);
    lock_guard<mutex> lock3(users_mutex);
    if (command.size() != 3)
        return "Usage : accept_request <group_id> <user_id>";
    if (portToUser.find(userPort) == portToUser.end() || !isAuth[portToUser[userPort]])
        return "You are not authorized, please login first";
    if (groups.find(command[1]) == groups.end())
        return "Group does not exist";
    if (groups[command[1]].owner.id != portToUser[userPort])
        return "You are not the owner of this group";
    auto it = find_if(groups[command[1]].requests.begin(), groups[command[1]].requests.end(), [&](const User &u)
                      { return u.id == command[2]; });
    if (it == groups[command[1]].requests.end())
        return "No such group request exists";
    groups[command[1]].requests.erase(it);
    groups[command[1]].members.push_back(users[command[2]]);
    return "Request accepted successfully" + change_marker;
}

string logout(vector<string> &command, int userPort)
{
    lock_guard<mutex> lock1(isAuth_mutex);
    lock_guard<mutex> lock2(portToUser_mutex);
    if (command.size() != 1)
        return "Usage : logout";
    if (portToUser.find(userPort) == portToUser.end() || !isAuth[portToUser[userPort]])
        return "You are not authorized, please login first";
    isAuth[portToUser[userPort]] = 0;
    portToUser.erase(userPort);
    return "Logout successfull" + change_marker;
}

void syncServer(vector<string> command, int userPort)
{
    int serverFD, portno, n, opt, socketFD;
    struct sockaddr_in serverAddress;
    struct hostent *server;
    server = gethostbyname("localhost");
    bzero((char *)&serverAddress, sizeof(serverAddress));
    serverAddress.sin_family = AF_INET;
    bcopy((char *)server->h_addr,
          (char *)&serverAddress.sin_addr.s_addr,
          server->h_length);
    socketFD = socket(AF_INET, SOCK_STREAM, 0);
    if (socketFD < 0)
        error("ERROR opening socket");
    portno = stoi(trackers[otherTrackerNumber - 1].second);

    serverAddress.sin_port = htons(portno);

    string res = change_marker;
    for (auto c : command)
        res += c + " ";
    res += to_string(userPort);
    char buffer[307200];

    if (connect(socketFD, (struct sockaddr *)&serverAddress, sizeof(serverAddress)) < 0)
    {
        lock_guard<mutex> lock(toSync_mutex);
        toSync.push_back({command, userPort});
        return;
    }
    if (!synced)
    {
        doSync = 1;
        synced = 1;
        toSync.push_back({command, userPort});
        char c[2];
        c[0] = '1';
        c[1] = '\0';
        n = write(socketFD, c, strlen(c));
        close(socketFD);
        for (auto a : toSync)
        {
            syncServer(a.first, a.second);
        }
        return;
    }

    bzero(buffer, sizeof(buffer));
    string sync_msg = res + "\n";
    strcpy(buffer, sync_msg.c_str());
    n = write(socketFD, buffer, strlen(buffer));
    close(socketFD);
}

string uploadFile(vector<string> &command, int userPort)
{
    if (command.size() < 7)
        return "Usage: upload_file <group_id> <file_name> <file_size> <file_hash> <chunk_hashes> <seeder_port>";
    string group_id = command[1];
    string file_path = command[2];
    long file_size = stol(command[3]);
    string file_hash = command[4];
    string chunk_hashes_str = command[5];
    int seeder_port = stoi(command[6]);
    string user;
    {
        lock_guard<mutex> lock(portToUser_mutex);
        if (portToUser.find(userPort) == portToUser.end())
            return "You are not authorized, please login first";
        user = portToUser[userPort];
        user_ports[user] = seeder_port; // Store user's download listener port
    }
    {
        lock_guard<mutex> lock(groups_mutex);
        if (groups.find(group_id) == groups.end())
            return "Group does not exist";
        bool is_member = false;
        for (auto &m : groups[group_id].members)
            if (m.id == user)
                is_member = true;
        if (!is_member)
            return "You are not a member of this group";
    }
    // Extract file name from file_path (after last '/')
    string file_name = file_path;
    size_t pos = file_path.find_last_of('/');
    if (pos != string::npos)
        file_name = file_path.substr(pos + 1);

    vector<string> chunk_hashes;
    char *chunk_str = strdup(chunk_hashes_str.c_str());
    char *token = strtok(chunk_str, ",");
    while (token)
    {
        chunk_hashes.push_back(string(token));
        token = strtok(NULL, ",");
    }
    free(chunk_str);

    {
        lock_guard<mutex> lock(groups_mutex);
        if (group_files.find(group_id) != group_files.end())
        {
            for (auto &f : group_files[group_id])
            {
                if (f.file_name == file_name)
                    return "File with same name already exists in this group";
            }
        }
    }

    FileInfo info(file_path, file_name, user, group_id, file_size, file_hash, chunk_hashes);
    group_files[group_id].push_back(info);
    // Mark uploader as seeder for the file
    file_seeders[group_id][file_name].push_back(user);
    seeder_file_paths[user][file_name] = file_path; // Register uploader's file path
    return "File uploaded and metadata stored" + change_marker;
}

string listFiles(vector<string> &command, int userPort)
{
    lock_guard<mutex> lock(groups_mutex);
    if (command.size() != 2)
        return "Usage: list_files <group_id>";
    string group_id = command[1];
    if (portToUser.find(userPort) == portToUser.end())
        return "You are not authorized, please login first";
    if (groups.find(group_id) == groups.end())
        return "Group does not exist";
    bool is_member = false;
    for (auto &m : groups[group_id].members)
        if (m.id == portToUser[userPort])
            is_member = true;
    if (!is_member)
        return "You are not a member of this group";
    if (group_files.find(group_id) == group_files.end() || group_files[group_id].empty())
        return "No files in this group";
    string result;
    for (auto &f : group_files[group_id])
    {
        result += f.file_name + "\n";
    }
    if (!result.empty())
        result.pop_back();
    return result;
}

string downloadFile(vector<string> &command, int userPort)
{
    if (command.size() != 4)
        return "Usage: download_file <group_id> <file_name> <destination_path>";
    string group_id = command[1];
    string file_name = command[2];
    string destination_path = command[3];

    if (portToUser.find(userPort) == portToUser.end())
        return "You are not authorized, please login first";
    if (groups.find(group_id) == groups.end())
        return "Group does not exist";
    bool is_member = false;
    for (auto &m : groups[group_id].members)
        if (m.id == portToUser[userPort])
            is_member = true;
    if (!is_member)
        return "You are not a member of this group";
    if (group_files.find(group_id) == group_files.end())
        return "No files in this group";
    FileInfo *file = nullptr;
    for (auto &f : group_files[group_id])
    {
        if (f.file_name == file_name)
        {
            file = &f;
            break;
        }
    }
    if (!file)
        return "File not found in this group";

    string result;
    result += "file_name:" + file->file_name + "\n";
    result += "file_size:" + to_string(file->file_size) + "\n";
    result += "file_hash:" + file->file_hash + "\n";
    result += "chunk_hashes:";
    for (size_t i = 0; i < file->chunk_hashes.size(); ++i)
    {
        result += file->chunk_hashes[i];
        if (i + 1 < file->chunk_hashes.size())
            result += ",";
    }
    result += "\n";
    vector<string> authed_seeders;
    for (auto &uid : file_seeders[group_id][file_name])
    {
        if (isAuth.find(uid) != isAuth.end() && isAuth[uid])
            authed_seeders.push_back(uid);
    }
    result += "seeders:";
    if (authed_seeders.empty())
        result += "None";
    else
        for (auto &uid : authed_seeders)
            result += uid + ",";
    if (!authed_seeders.empty())
        result.pop_back();
    result += "\n";
    result += "seeder_ports:";
    if (authed_seeders.empty())
        result += "None";
    else
    {
        for (auto &uid : authed_seeders)
        {
            if (user_ports.find(uid) != user_ports.end())
                result += to_string(user_ports[uid]) + ",";
            else
                result += "0,";
        }
        if (!authed_seeders.empty())
            result.pop_back();
    }
    result += "\n";
    result += "seeder_file_paths:";
    if (authed_seeders.empty())
        result += "None";
    else
    {
        for (auto &uid : authed_seeders)
        {
            if (seeder_file_paths.find(uid) != seeder_file_paths.end() && seeder_file_paths[uid].find(file_name) != seeder_file_paths[uid].end())
                result += seeder_file_paths[uid][file_name] + ",";
            else
                result += "unknown,";
        }
        if (!authed_seeders.empty())
            result.pop_back();
    }
    result += "\n";
    return result;
}

string handleNewSeeder(vector<string> &command, int userPort)
{
    if (command.size() != 5)
        return "Usage: new_seeder <group_id> <file_name> <file_path> <seeder_port>";
    string group_id = command[1];
    string file_name = command[2];
    string file_path = command[3];
    int seeder_port = stoi(command[4]);
    string user;
    {
        lock_guard<mutex> lock(portToUser_mutex);
        if (portToUser.find(userPort) == portToUser.end())
            return "You are not authorized, please login first";
        user = portToUser[userPort];
        user_ports[user] = seeder_port;
    }
    {
        lock_guard<mutex> lock(groups_mutex);
        if (groups.find(group_id) == groups.end())
            return "Group does not exist";
        bool is_member = false;
        for (auto &m : groups[group_id].members)
            if (m.id == user)
                is_member = true;
        if (!is_member)
            return "You are not a member of this group";
    }
    // Add user as seeder for the file
    file_seeders[group_id][file_name].push_back(user);
    seeder_file_paths[user][file_name] = file_path; // Store the file path for this seeder
    return "Seeder registered" + change_marker;
}

string stopShare(vector<string> &command, int userPort)
{
    if (command.size() != 3)
        return "Usage: stop_share <group_id> <file_name>";
    string group_id = command[1];
    string file_name = command[2];
    string user;

    {
        lock_guard<mutex> lock_port(portToUser_mutex);
        if (portToUser.find(userPort) == portToUser.end())
            return "You are not authorized, please login first";
        user = portToUser[userPort];
    }

    {
        lock_guard<mutex> lock_groups(groups_mutex);
        if (groups.find(group_id) == groups.end())
            return "Group does not exist";
        bool is_member = false;
        for (auto &m : groups[group_id].members)
            if (m.id == user)
                is_member = true;
        if (!is_member)
            return "You are not a member of this group";

        // Remove user from seeders list for this file
        auto &seeders = file_seeders[group_id][file_name];
        auto it = std::find(seeders.begin(), seeders.end(), user);
        if (it != seeders.end())
            seeders.erase(it);
        if (seeder_file_paths.find(user) != seeder_file_paths.end())
            seeder_file_paths[user].erase(file_name);
    }

    return "Stopped sharing file" + change_marker;
}