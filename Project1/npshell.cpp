#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <map>
#include <list>
using namespace std;

int command_count = 0;
multimap<int, int> numbered_pipe_fds; // Store file descriptors instead of content

// type: 0="|", 1="|N", 2="!N", -1=not a pipe
pair<int, int> parse_pipe(const string& s) {
    if (s == "|") return {0, 0};
    if (s[0] == '|' && s.size() > 1) {
        int num = 0;
        for (int i = 1; i < s.size(); i++) {
            if (s[i] < '0' || s[i] > '9') return {-1, -1};
            num = num * 10 + s[i] - '0';
        }
        return {1, num};
    }
    if (s[0] == '!' && s.size() > 1) {
        int num = 0;
        for (int i = 1; i < s.size(); i++) {
            if (s[i] < '0' || s[i] > '9') return {-1, -1};
            num = num * 10 + s[i] - '0';
        }
        return {2, num};
    }
    return {-1, -1};
}

// Handle direct execution of a command
void execute_direct_command(const vector<string>& args, int input_fd, int output_fd, bool merge_stderr) {
    // Handle output redirection
    int redirect_index = -1;
    string output_file;
    
    vector<string> exec_args = args;
    for (int i = 0; i < exec_args.size(); ++i) {
        if (exec_args[i] == ">") {
            redirect_index = i;
            if (i + 1 < exec_args.size()) {
                output_file = exec_args[i + 1];
            }
            break;
        }
    }
    
    // Remove redirection markers
    if (redirect_index != -1) {
        exec_args.erase(exec_args.begin() + redirect_index, exec_args.end());
    }
    
    // Prepare exec parameters
    vector<char*> c_args;
    for (const auto& arg : exec_args) {
        c_args.push_back(const_cast<char*>(arg.c_str()));
    }
    c_args.push_back(nullptr);
    
    // Handle input redirection
    if (input_fd != STDIN_FILENO) {
        dup2(input_fd, STDIN_FILENO);
        close(input_fd);
    }
    
    // Handle output redirection
    if (redirect_index != -1) {
        int fd = open(output_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd == -1) {
            cerr << "Cannot open output file: " << output_file << '\n';
            exit(1);
        }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    } else if (output_fd != STDOUT_FILENO) {
        dup2(output_fd, STDOUT_FILENO);
    }
    
    // Handle stderr merging
    if (merge_stderr) {
        dup2(STDOUT_FILENO, STDERR_FILENO);
    }
    
    // Execute command
    execvp(c_args[0], c_args.data());
    cerr << "Unknown command: [" << c_args[0] << "].\n";
    exit(1);
}

// Process command sequence
void process_commands(vector<string> commands) {
    if (commands.empty()) return;
    
    // Handle multiple input pipes
    vector<int> input_fds;
    if (numbered_pipe_fds.count(command_count) > 0) {
        auto range = numbered_pipe_fds.equal_range(command_count);
        for (auto it = range.first; it != range.second; ++it) {
            input_fds.push_back(it->second);
        }
        numbered_pipe_fds.erase(command_count);
    }
    
    int input_fd = STDIN_FILENO;
    
    // If there are multiple input pipes, need to merge them
    if (!input_fds.empty()) {
        if (input_fds.size() == 1) {
            // If only one input pipe, use it directly
            input_fd = input_fds[0];
        } else {
            // If multiple input pipes, need to merge them
            // Create a temporary pipe for merging multiple inputs
            int merge_pipe[2];
            if (pipe(merge_pipe) == -1) {
                cerr << "Merge pipe creation failed\n";
                return;
            }
            
            // For each input pipe, fork a process to read and write to the merge pipe
            for (int fd : input_fds) {
                pid_t pid;
                while ((pid = fork()) == -1) {
                    if (errno == EAGAIN) {
                        usleep(1000);
                    } else {
                        exit(1);
                    }
                }
                if (pid == 0) {  // Child process
                    close(merge_pipe[0]);  // Close the read end of the merge pipe
                    
                    // Write the contents of the input pipe to the merge pipe
                    char buffer[4096];
                    ssize_t n;
                    while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
                        write(merge_pipe[1], buffer, n);
                    }
                    
                    close(fd);
                    close(merge_pipe[1]);
                    exit(0);
                } else if (pid > 0) {  // Parent process
                    close(fd);  // Close this input pipe in the parent process
                } else {
                    cerr << "Fork failed\n";
                    return;
                }
            }
            
            // Close the write end of the merge pipe, so the read end will get EOF when all child processes finish
            close(merge_pipe[1]);
            
            // Use the read end of the merge pipe as input
            input_fd = merge_pipe[0];
            
            // Wait for all child processes to finish
            for (size_t i = 0; i < input_fds.size(); i++) {
                wait(NULL);
            }
        }
    }
    
    size_t i = 0;
    while (i < commands.size()) {
        // Get current command
        vector<string> current_cmd;
        while (i < commands.size()) {
            auto [pipe_type, pipe_num] = parse_pipe(commands[i]);
            if (pipe_type != -1) break;
            current_cmd.push_back(commands[i]);
            i++;
        }
        
        // Check if there's a pipe
        if (i < commands.size()) {
            auto [pipe_type, pipe_num] = parse_pipe(commands[i]);
            i++; // Move past pipe symbol
            
            if (pipe_type == 0) {  // Normal pipe
                int pipe_fd[2];
                if (pipe(pipe_fd) == -1) {
                    cerr << "Pipe creation failed\n";
                    return;
                }
                
                // Use fork + dup2 for normal pipes
                pid_t pid;
                while ((pid = fork()) == -1) {
                    if (errno == EAGAIN) {
                        usleep(1000);
                    } else {
                        exit(1);
                    }
                }
                if (pid == 0) {  // Child process
                    close(pipe_fd[0]);  // Close read end in child
                    execute_direct_command(current_cmd, input_fd, pipe_fd[1], false);
                    // Should not reach here
                    exit(1);
                } else if (pid > 0) {  // Parent process
                    // Close write end, prepare read end for next command
                    close(pipe_fd[1]);
                    if (input_fd != STDIN_FILENO) close(input_fd);
                    input_fd = pipe_fd[0];
                    
                    // We don't wait for the child process to finish
                    // to allow pipeline to flow naturally
                } else {
                    cerr << "Fork failed\n";
                    return;
                }
            } else {  // Numbered pipe (|n or !n)
                int target_cmd = command_count + pipe_num;
                bool merge_stderr = (pipe_type == 2);  // !n type
                
                // Create pipe for the numbered pipe
                int numbered_pipe_fd[2];
                if (pipe(numbered_pipe_fd) == -1) {
                    cerr << "Numbered pipe creation failed\n";
                    return;
                }
                
                pid_t pid;
                while ((pid = fork()) == -1) {
                    if (errno == EAGAIN) {
                        usleep(1000);
                    } else {
                        exit(1);
                    }
                }
                if (pid == 0) {  // Child process
                    close(numbered_pipe_fd[0]);  // Close read end in child
                    
                    // Execute command with stdout (and maybe stderr) to the pipe
                    execute_direct_command(current_cmd, input_fd, numbered_pipe_fd[1], merge_stderr);
                    // Should not reach here
                    exit(1);
                } else if (pid > 0) {  // Parent process
                    // Close write end in parent
                    close(numbered_pipe_fd[1]);
                    
                    numbered_pipe_fds.insert({target_cmd, numbered_pipe_fd[0]});
                    
                    // Reset input for next command
                    if (input_fd != STDIN_FILENO) close(input_fd);
                    input_fd = STDIN_FILENO;
                    
                    // We don't wait for the child to finish
                } else {
                    cerr << "Fork failed\n";
                    return;
                }
            }
        } else {
            // Last command, execute directly
            pid_t pid;
            while ((pid = fork()) == -1) {
                if (errno == EAGAIN) {
                    usleep(1000);
                } else {
                    exit(1);
                }
            }
            if (pid == 0) {  // Child process
                execute_direct_command(current_cmd, input_fd, STDOUT_FILENO, false);
                // Should not reach here
                exit(1);
            } else if (pid > 0) {  // Parent process
                if (input_fd != STDIN_FILENO) close(input_fd);
                
                // Wait for the last command to finish
                int status;
                waitpid(pid, &status, 0);
            } else {
                cerr << "Fork failed\n";
                return;
            }
        }
    }
    
    // Clean up any remaining input file descriptors
    if (input_fd != STDIN_FILENO) close(input_fd);
}

// Signal handler for zombie processes
void sigchld_handler(int signo) {
    // Reap all dead processes
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

int main() {
    // Setup signal handler to prevent zombie processes
    signal(SIGCHLD, sigchld_handler);
    
    setenv("PATH", "bin:.", 1);
    string input_line;
    vector<string> pending_commands;
    
    while (true) {
        vector<string> commands;
        
        if (!pending_commands.empty()) {
            commands = pending_commands;
            pending_commands.clear();
        } else {
            cout << "% ";
            getline(cin, input_line);
            istringstream iss(input_line);
            string word;
            while (iss >> word) {
                commands.push_back(word);
                
                // Check for numbered pipe, put subsequent commands in pending
                auto [pipe_type, pipe_num] = parse_pipe(word);
                if (pipe_type == 1 || pipe_type == 2) {
                    while (iss >> word) {
                        pending_commands.push_back(word);
                    }
                    break;
                }
            }
        }
        
        if (commands.empty()) continue;
        command_count++;
        
        if (commands[0] == "exit") {
            // Close all open file descriptors
            for (const auto& [cmd, fd] : numbered_pipe_fds) {
                close(fd);
            }
            break;
        } else if (commands[0] == "setenv") {
            if (commands.size() >= 3) {
                setenv(commands[1].c_str(), commands[2].c_str(), 1);
            } else {
                cerr << "setenv: not enough arguments\n";
            }
        } else if (commands[0] == "printenv") {
            if (commands.size() >= 2) {
                const char* value = getenv(commands[1].c_str());
                if (value) {
                    cout << value << '\n';
                }
            } else {
                cerr << "printenv: not enough arguments\n";
            }
        } else {
            process_commands(commands);
        }
    }
    
    return 0;
}
