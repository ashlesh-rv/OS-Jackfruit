OS-Jackfruit: Mini Container Runtime
README

------------------------------------------------------------
1. Team Information
------------------------------------------------------------

Name : Ashlesh RV  
SRN  : PES1UG24CS088  

Name : B Varun  
SRN  : PES1UG24CS109  


------------------------------------------------------------
2. Build, Load, and Run Instructions
------------------------------------------------------------

Step 1: Install Dependencies  
sudo apt update  
sudo apt install -y build-essential linux-headers-$(uname -r)  

Step 2: Build the Project  
cd boilerplate  
make clean  
make  

Step 3: Build Workload  
gcc -O2 -Wall -static -o cpu_hog cpu_hog.c  

Step 4: Copy Workload into Rootfs  
cp cpu_hog rootfs-alpha/  
cp cpu_hog rootfs-beta/  
chmod +x rootfs-alpha/cpu_hog  
chmod +x rootfs-beta/cpu_hog  

Step 5: Load Kernel Module  
sudo insmod monitor.ko  
lsmod | grep monitor  

Step 6: Run Containers  
sudo ./engine start alpha ../rootfs-alpha dummy  
sudo ./engine start beta ../rootfs-beta dummy  

Step 7: Check Containers  
sudo ./engine ps  


------------------------------------------------------------
3. Demo with Screenshots
------------------------------------------------------------

Screenshot 1: Multi-Container Runtime  

(Insert screenshot showing two cpu_hog processes running)<img width="952" height="213" alt="image" src="https://github.com/user-attachments/assets/9d1576a2-8be4-4575-a8e8-3fb04e6fe908" />


Screenshot 2: CLI + IPC  
(Insert screenshot showing start, already running, stop, ps)

Screenshot 3: Logging Output  
(Insert screenshot of logs.txt output)

Screenshot 4: Kernel Monitor  
(Insert screenshot of lsmod and dmesg output)

Screenshot 5: Scheduling Experiment  
(Insert screenshot of top showing cpu_hog using CPU)

Screenshot 6: Cleanup  
(Insert screenshot showing no cpu_hog processes)


------------------------------------------------------------
4. Engineering Analysis
------------------------------------------------------------

4.1 Isolation Mechanisms  
The system provides basic isolation using Linux process separation. Each container is created using fork() and exec(), allowing workloads to run as independent processes. Although full namespace isolation (such as CLONE_NEWPID or CLONE_NEWNS) is not implemented, each container runs separately without interfering with others. All containers share the host kernel, similar to lightweight container environments.

4.2 Supervisor and Process Lifecycle  
The engine acts as a controller for managing container lifecycle. It handles operations such as starting, stopping, and listing containers. Each container is associated with a process ID (PID), which is tracked internally. The lifecycle includes states such as running and stopped. Processes are terminated using signals, ensuring proper cleanup and avoiding leftover processes.

4.3 IPC, Threads, and Synchronization  
The system uses a simple CLI-based control mechanism instead of complex IPC. Commands are directly handled by the engine, and no separate communication channels like sockets or pipes are used. The implementation does not require multithreading or synchronization primitives since operations are handled sequentially. This simplifies the design while still demonstrating control flow.

4.4 Memory Management and Enforcement  
Memory management is handled by the Linux operating system. The project does not enforce strict memory limits using cgroups. However, the kernel module demonstrates how monitoring can be done at the kernel level. This highlights how real systems track memory usage and enforce limits to maintain system stability.

4.5 Scheduling Behavior  
Linux uses the Completely Fair Scheduler (CFS) to allocate CPU time among processes. In this project, CPU-intensive workloads (cpu_hog) are used to simulate scheduling behavior. When multiple containers run simultaneously, CPU resources are shared fairly. This is observed using tools like top, where multiple processes consume CPU concurrently.


------------------------------------------------------------
5. Design Decisions and Tradeoffs
------------------------------------------------------------

• Used process-based isolation instead of namespaces to reduce complexity  
• Implemented CLI-based control instead of advanced IPC mechanisms  
• Used static binaries to avoid dependency issues inside minimal environments  
• Logging implemented using file redirection for simplicity  
• Kernel module used only for monitoring rather than strict enforcement  


------------------------------------------------------------
6. Scheduler Experiment Results
------------------------------------------------------------

Experiment    Container    Behavior  
A             alpha        High CPU usage  
A             beta         High CPU usage  

Analysis  
Both containers compete for CPU resources when running simultaneously.  
The scheduler distributes CPU time fairly between them. This demonstrates  
how the Linux scheduler handles multiple CPU-bound processes.


------------------------------------------------------------
7. Conclusion
------------------------------------------------------------

This project demonstrates how a simple container runtime can be built using  
Linux system calls such as fork() and exec(). It highlights process  
management, logging, monitoring, and scheduling behavior. While simplified,  
the implementation provides a strong understanding of how real container  
systems operate and can be extended with advanced features such as  
namespaces and resource control.
