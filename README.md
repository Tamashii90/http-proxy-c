# http-proxy-c

An HTTP 1.1 proxy created for educational goals.  
**Skeleton code (init commit) taken from Berkeley's [CS 162](https://cs162.org/).**

The server has three modes:

- #### fork-server
  Forks a new process for each connection.

- #### thread-server
  Creates a new thread for each connection.

- #### pool-server
  Creates a pool of n threads that share a queue. The main thread loops and accepts new connection requests from clients and pushes each new connection file-descriptor onto the queue. Each thread will pop a connection off the queue and handle it, or will sleep until a new one comes in.  


### Examples

- #### pool-server

  https://user-images.githubusercontent.com/75096284/224495339-5e407f58-1f29-4a2b-8e98-6993df84822a.mp4

  Because the number of threads is small (5), and because the connections are **persistent**, this results in noticeable delays because the proxy server can't serve a new connection until an old one times out.

- #### thread-server

  https://user-images.githubusercontent.com/75096284/224495638-9e9c9100-baee-4949-888c-36ccc611a920.mp4

  Unlike the previous example, there's no limit to the number of threads created, resulting in much faster response times.
