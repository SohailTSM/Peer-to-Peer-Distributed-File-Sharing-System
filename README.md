# P2P Distributed File Sharing System

## Overview

This project implements a peer-to-peer (P2P) file sharing system with tracker-based coordination. The system consists of two main components:

- **Client**: Used by users to interact with the system, upload/download files, and join groups.
- **Tracker**: Manages users, groups, file metadata, and coordinates file sharing between clients.

## Execution Steps

### 1. Compile the Code

```bash
cd tracker
g++ tracker.cpp
cd ../client
g++ client.cpp -lssl -lcrypto
```

### 2. Start Trackers

Each tracker is started with its tracker number and the tracker info file:

```bash
cd tracker
./a.out ../tracker_info.txt 1 # Tracker 1
./a.out ../tracker_info.txt 2 # Tracker 2
```

### 3. Start Clients

Each client is started with the IP:PORT and tracker info file:

```bash
cd client
./a.out 127.0.0.1:7001 ../tracker_info.txt
./a.out 127.0.0.1:7002 ../tracker_info.txt
./a.out 127.0.0.1:7003 ../tracker_info.txt
```

## Client Entered Commands

These are commands the user can enter at the client prompt (`$>`):

### User and Group Management

- `create_user <user_id> <password>`: Register a new user.
- `login <user_id> <password>`: Log in as a user.
- `create_group <group_id>`: Create a new group.
- `join_group <group_id>`: Request to join a group.
- `leave_group <group_id>`: Leave a group.
- `list_groups`: List all groups.
- `list_requests <group_id>`: List join requests for a group (owner only).
- `accept_request <group_id> <user_id>`: Accept a user's join request (owner only).
- `logout`: Log out the current user.

### File Operations

- `upload_file <group_id> <file_path>`: Upload a file to a group. Computes file and chunk hashes, sends metadata to tracker.
- `list_files <group_id>`: List files available in a group.
- `download_file <group_id> <file_name> <destination_path>`: Download a file from seeders in the group. Downloads in chunks using multiple threads.
- `stop_share <group_id> <file_name>`: Stop sharing a file as a seeder.
- `exit`: Exit the client.

## Internal Commands

### Client to Tracker

- `upload_file <group_id> <file_path> <file_size> <file_hash> <chunk_hashes> <my_port>`: Sent when uploading a file. Registers file metadata and seeder info.
- `new_seeder <group_id> <file_name> <file_path> <my_port>`: Notifies tracker that client is now a seeder for a file after download.

### Tracker to Client

- Metadata response for `download_file` includes:
  - `file_name:<name>`
  - `file_size:<size>`
  - `file_hash:<hash>`
  - `chunk_hashes:<hash1,hash2,...>`
  - `seeders:<user1,user2,...>`
  - `seeder_ports:<port1,port2,...>`
  - `seeder_file_paths:<path1,path2,...>`

### Client to Client

- For downloading file chunks, clients send:
  - `GET_CHUNK <chunk_index> <file_path>`: Requests a specific chunk from a seeder's download listener.
  - Seeder responds with the chunk data.

### Server to Server (Tracker Sync)

- Trackers synchronize state using commands prefixed with a change marker (`123456789`).
- Sync commands mirror user actions (e.g., `create_user`, `upload_file`, etc.) and are sent to the other tracker for redundancy.
- Example sync command: `123456789 upload_file ... <userPort>`

## File Structure

- `client/client.cpp`: Client implementation.
- `tracker/tracker.cpp`: Tracker implementation.
- `tracker_info.txt`: Contains tracker IP and port info.

## Notes

- All network communication uses TCP sockets.
- File integrity is ensured using SHA1 hashes for files and chunks.
- Multi-threaded chunk downloading for performance.
- Trackers maintain group, user, and file metadata, and synchronize for fault tolerance.

## Data Structures Used

### Client Data Structures

- `vector<pair<string, string>> completed_downloads`: Stores information about completed downloads (file name and path).
- `mutex completed_downloads_mutex`: Ensures thread-safe access to `completed_downloads`.

#### Download Listener & Chunk Management

- Uses vectors and atomic variables for multi-threaded chunk downloading:
  - `vector<vector<char>> chunk_datas`: Stores data for each chunk being downloaded.
  - `vector<bool> chunk_ok`: Flags indicating if a chunk was successfully downloaded.
  - `mutex write_mutex`: Ensures thread-safe writes to the output file.
  - `atomic<int> next_chunk`: Used for thread pool chunk assignment.

### Server (Tracker) Data Structures

- `map<string, User> users`: Maps user IDs to `User` objects (stores credentials).
- `map<string, Group> groups`: Maps group IDs to `Group` objects (stores group info, members, requests, owner).
- `map<string, int> isAuth`: Tracks authentication status of users.
- `map<int, string> portToUser`: Maps client port numbers to user IDs for session management.
- `vector<pair<string, string>> trackers`: Stores tracker IP and port info.
- `vector<pair<vector<string>, int>> toSync`: Stores commands to be synchronized with the other tracker.
- `mutex users_mutex`, `groups_mutex`, `isAuth_mutex`, `portToUser_mutex`, `toSync_mutex`: Ensure thread-safe access to shared data.

#### File Sharing Metadata

- `class FileInfo`: Stores metadata for each shared file (name, path, owner, group, size, hash, chunk hashes).
- `map<string, vector<FileInfo>> group_files`: Maps group IDs to lists of files shared in the group.
- `map<string, map<string, vector<string>>> file_seeders`: Maps group IDs and file names to lists of user IDs who are seeders.
- `map<string, int> user_ports`: Maps user IDs to their download listener port numbers.
- `map<string, map<string, string>> seeder_file_paths`: Maps user IDs and file names to the file paths on the seeder's machine.

---

