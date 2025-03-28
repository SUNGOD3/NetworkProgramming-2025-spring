#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <queue>
#include <map>
using namespace std;

int num_commands = 0;
map<int, string> number_pipe_outputs;

void execute_single_command(const vector<string>& full_args, int input_fd = STDIN_FILENO, int output_fd = STDOUT_FILENO, bool merge_stderr = false) {
    // Find redirection
    int redirect_index = -1;
    string output_file;
    vector<string> args = full_args;
    for (int i = 0; i < args.size(); ++i) {
        if (args[i] == ">") {
            redirect_index = i;
            if (i + 1 < args.size()) {
                output_file = args[i + 1];
            }
            break;
        }
    }

    // Remove redirection tokens if present
    if (redirect_index != -1) {
        args.erase(args.begin() + redirect_index, args.end());
    }

    // Prepare arguments for exec
    vector<char*> exec_args;
    for (const auto& arg : args) {
        exec_args.push_back(const_cast<char*>(arg.c_str()));
    }
    exec_args.push_back(nullptr);

    // Fork and execute
    pid_t pid = fork();
    if (pid == 0) {  // Child process
        // Redirect input if necessary
        if (input_fd != STDIN_FILENO) {
            dup2(input_fd, STDIN_FILENO);
            close(input_fd);
        }

        // Redirect output if necessary
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
            close(output_fd);
        }

        // Merge stderr if required
        if (merge_stderr) {
            dup2(STDOUT_FILENO, STDERR_FILENO);
        }

        // Execute command
        execvp(exec_args[0], exec_args.data());
        
        // If execvp fails
        cerr << "Unknown command: [" << exec_args[0] << "]\n";
        exit(1);
    } else if (pid > 0) {  // Parent process
        // Close file descriptors
        if (input_fd != STDIN_FILENO) close(input_fd);
        if (output_fd != STDOUT_FILENO) close(output_fd);

        int status;
        waitpid(pid, &status, 0);
    } else {
        cerr << "Fork failed\n";
    }
}
int is_stderr_pipe(const string& s) {
    if (s[0] == '!' && s.size() > 1) {
        int num = 0;
        for (int i = 1; i < s.size(); i++) {
            if (s[i] < '0' || s[i] > '9') return -1;
            num = num * 10 + s[i] - '0';
        }
        return num;
    }
    return -1;
}
int is_pipe(const string& s) {
    if (s == "|") return 0;
    else if (s[0] == '|' && s.size() > 1) {
        int num = 0;
        for (int i = 1; i < s.size(); i++) {
            if (s[i] < '0' || s[i] > '9') return -1;
            num = num * 10 + s[i] - '0';
        }
        return num;
    }
    else {
        return -1;
    }
}

void exec(const vector<string>& commands, int& prev_pipe_read) {
    int n = commands.size();
    
    for (int i = 0; i < n;) {
        // Find the next pipe or end of command
        int j = i;
        bool is_merge_stderr = false;
        while (j < n && is_pipe(commands[j]) == -1 && is_stderr_pipe(commands[j]) == -1) {
            j++;
        }
        
        // Extract current command
        vector<string> current_command(commands.begin() + i, commands.begin() + j);
        
        if (j < n && (is_pipe(commands[j]) > 0 || is_stderr_pipe(commands[j]) > 0)) {
            int pipe_num;
            bool is_stderr_merge = false;
            
            if (is_pipe(commands[j]) > 0) {
                pipe_num = num_commands + is_pipe(commands[j]);
            } else {
                pipe_num = num_commands + is_stderr_pipe(commands[j]);
                is_stderr_merge = true;
            }
            
            // Create pipe
            int pipe_fd[2];
            if (pipe(pipe_fd) == -1) {
                cerr << "Pipe creation failed\n";
                return;
            }

            // Fork to execute current command
            pid_t pid = fork();
            if (pid == 0) {  // Child process
                close(pipe_fd[0]);
                
                // Redirect input if needed
                if (prev_pipe_read != STDIN_FILENO) {
                    dup2(prev_pipe_read, STDIN_FILENO);
                    close(prev_pipe_read);
                }

                // Redirect output to pipe
                dup2(pipe_fd[1], STDOUT_FILENO);
                
                // Merge stderr if required
                if (is_stderr_merge) {
                    dup2(pipe_fd[1], STDERR_FILENO);
                }
                close(pipe_fd[1]);

                // Execute command
                vector<char*> exec_args;
                for (const auto& arg : current_command) {
                    exec_args.push_back(const_cast<char*>(arg.c_str()));
                }
                exec_args.push_back(nullptr);
                
                execvp(exec_args[0], exec_args.data());
                
                // If execvp fails
                cerr << "Unknown command: [" << exec_args[0] << "]\n";
                exit(1);
            } else if (pid > 0) {  // Parent process
                close(pipe_fd[1]);
                
                // Read output and store
                char buffer[4096];
                string output;
                ssize_t bytes_read;
                while ((bytes_read = read(pipe_fd[0], buffer, sizeof(buffer))) > 0) {
                    output.append(buffer, bytes_read);
                }
                close(pipe_fd[0]);

                // Wait for child to finish
                int status;
                waitpid(pid, &status, 0);

                // Store output for the specified numbered pipe
                number_pipe_outputs[pipe_num] += output;

                // Prepare for next iteration
                i = j + 1;
                continue;
            } else {
                cerr << "Fork failed\n";
                return;
            }
        }
        else if(j < n) {
            // We have a pipe, create a pipe
            int pipe_fd[2];
            if (pipe(pipe_fd) == -1) {
                cerr << "Pipe creation failed\n";
                return;
            }
            // Execute current command with previous pipe input and current pipe output
            execute_single_command(current_command, prev_pipe_read, pipe_fd[1], is_merge_stderr);

            // Close write end of pipe
            close(pipe_fd[1]);

            // Prepare for next iteration
            prev_pipe_read = pipe_fd[0];
            i = j + 1;
        }
        else {  
            // Last command or no pipe
            execute_single_command(current_command, prev_pipe_read);
            prev_pipe_read = STDIN_FILENO;
            break;
        }
    }
}

int main() {
    setenv("PATH", "bin:.", 1);
    string s;
    int prev_pipe_read = STDIN_FILENO;
    while (1) {
        cout << "% ";
        getline(cin, s);
        vector<string> commands;
        istringstream iss(s);
        string word;
        while (iss >> word) {
            commands.push_back(word);
        }
        
        if (commands.empty()) continue;
        ++num_commands;
        
        // Check if we need to output stored number pipe content
        if (number_pipe_outputs.count(num_commands)) {
            int pipe_fd[2];
            if (pipe(pipe_fd) == -1) {
                cerr << "Pipe creation failed\n";
                continue;
            }

            // Write stored output to pipe
            write(pipe_fd[1], number_pipe_outputs[num_commands].c_str(), 
                  number_pipe_outputs[num_commands].length());
            close(pipe_fd[1]);

            // Use this pipe as input for subsequent commands
            prev_pipe_read = pipe_fd[0];
        }
        
        if (commands[0] == "exit") {
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
            exec(commands, prev_pipe_read);
        }
    }
    return 0;
}