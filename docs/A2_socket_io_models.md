# Part A-2. 소켓 I/O 모델의 진화

수만 명의 동시 접속자를 처리해야 하는 게임 서버에서 **"어떤 방식으로 소켓 I/O를 처리할 것인가?"**는 서버의 전체 아키텍처와 최대 처리량(Throughput)을 결정짓는 가장 근본적인 문제다.

초창기 C10K(동시 접속 1만 명) 문제를 해결하기 위해 소켓 I/O 모델이 어떻게 진화해 왔는지, 그리고 학계와 실무에서 자주 혼동하는 **Blocking / Non-Blocking**과 **Synchronous / Asynchronous**의 명확한 기준을 정리한다.

---

## 1. 네트워크 Read I/O의 2단계 과정

모든 소켓 수신(`recv`, `read`) 작업은 커널 내부에서 다음 **2가지 단계(Phase)**를 거쳐 완료된다. 이 두 단계를 누가, 어떻게 처리하느냐에 따라 I/O 모델이 구분된다.

1. **Phase 1. 데이터 대기 (Wait for Data)**:
   - 네트워크 카드(NIC)를 통해 패킷이 도착하여 커널의 소켓 수신 버퍼(`Socket Recv Buffer`)에 데이터가 준비될 때까지 기다리는 단계.
2. **Phase 2. 데이터 복사 (Copy Data from Kernel to User)**:
   - 커널 수신 버퍼에 도착한 데이터를 유저 공간(User Space)의 애플리케이션 버퍼(`char buf[]`)로 복사하는 단계.

---

## 2. Blocking vs Non-Blocking I/O

**구분 기준**: 시스템 콜을 호출했을 때, 커널에 당장 처리할 데이터(또는 여유 버퍼)가 없더라도 **호출한 스레드의 제어권(Control)을 즉시 반환하는가, 아니면 스레드를 대기 상태(Sleep/Wait)로 전환하는가?**

### 2.1 Blocking I/O (블로킹 I/O)

소켓을 생성하면 기본적으로 Blocking 모드로 동작한다.

```text
User Thread                                   Kernel
    |                                            |
    | --- recv(fd, buf, ...) 호출 -------------> | [Phase 1: 패킷 도착 대기]
    | (스레드 Blocked / Context Switch 발생)     | ...
    |                                            | (패킷 도착!)
    |                                            | [Phase 2: 커널 버퍼 -> 유저 버퍼 복사]
    | <--- 복사 완료 후 스레드 깨움 (Return) --- |
```

- **동작 특징**: `recv()` 호출 시 **Phase 1(데이터 대기)**과 **Phase 2(데이터 복사)**가 모두 끝날 때까지 유저 스레드는 블로킹(Wait Queue로 이동)된다.
- **한계점 (Thread-per-Client 모델의 붕괴)**:
  - 소켓 하나당 `recv()`에서 스레드가 멈추므로, 동시 접속자 $N$명을 처리하려면 $N$개의 스레드가 필요하다.
  - 만약 동접 5,000명이라면 스레드 5,000개가 생성되는데, 스레드 하나당 기본 스택 메모리(Windows 1MB, Linux 8MB)만 계산해도 수 GB~수십 GB의 메모리가 낭비되며, 매 순간 발생하는 **Context Switching 비용** 때문에 CPU가 실제 로직은 처리하지 못하고 스케줄링만 하다가 서버가 마비된다.

---

### 2.2 Non-Blocking I/O (논블로킹 I/O)

소켓에 `fcntl(fd, F_SETFL, O_NONBLOCK)` (Linux) 또는 `ioctlsocket(sock, FIONBIO, &mode)` (Windows) 옵션을 설정한 상태다.

```text
User Thread                                   Kernel
    |                                            |
    | --- recv() 호출 -------------------------> | 데이터 없음!
    | <-- 즉시 반환 (-1, EWOULDBLOCK) ---------- |
    |                                            |
    | --- recv() 호출 (폴링) ------------------> | 데이터 없음!
    | <-- 즉시 반환 (-1, EWOULDBLOCK) ---------- | (이 사이에 패킷 도착)
    |                                            |
    | --- recv() 호출 -------------------------> | 데이터 있음!
    | (복사하는 동안만 잠시 대기)                 | [Phase 2: 커널 버퍼 -> 유저 버퍼 복사]
    | <-- 읽은 바이트 수 반환 ------------------ |
```

- **동작 특징**:
  - **Phase 1(데이터 대기)**에서 스레드를 블로킹하지 않는다. 커널 수신 버퍼에 데이터가 없으면 즉시 `-1`(`SOCKET_ERROR`)을 반환하고 에러 코드로 **`EWOULDBLOCK` (또는 `EAGAIN`, Windows는 `WSAEWOULDBLOCK`)**을 설정한다.
  - 이는 진짜 에러가 아니라 **"지금은 읽을 데이터가 없으니 블로킹하지 않고 그냥 리턴한다. 나중에 다시 시도해라"**라는 정상적인 상태 통보다.
- **코드 작성 패턴**:
  ```cpp
  int32_t recvLen = ::recv(socket, buffer, bufferSize, 0);
  if (recvLen == SOCKET_ERROR)
  {
      int32_t errCode = ::WSAGetLastError(); // Linux는 errno
      if (errCode == WSAEWOULDBLOCK)
      {
          // 정상 상황: 아직 수신된 데이터가 없음. 다음 루프에서 처리
          return;
      }
      // 진짜 에러 발생: 연결 종료 처리
      Disconnect();
      return;
  }
  else if (recvLen == 0)
  {
      // 상대방이 FIN을 보냄 (Graceful Close)
      Disconnect();
      return;
  }
  ```
- **단독 사용 시 한계점 (Busy Waiting)**:
  - 스레드 하나로 1,000개의 Non-Blocking 소켓을 `while`문으로 돌며 계속 `recv()`를 찔러보는(Polling) 방식을 쓰면, 아무도 패킷을 보내지 않는 상황에서도 **CPU 코어 사용률이 100%로 치솟고** 불필요한 시스템 콜 전환 비용이 무한히 발생한다.
  - 또한 데이터가 도착했을 때 **Phase 2(커널 $\rightarrow$ 유저 버퍼 복사)**를 수행하는 동안에는 여전히 호출 스레드가 직접 복사를 마칠 때까지 기다려야 한다(즉, Non-Blocking I/O도 **Synchronous I/O**에 속한다).

---

## 3. Synchronous vs Asynchronous I/O (Reactor vs Proactor)

Richard Stevens의 *UNIX Network Programming* 및 POSIX 표준에 따른 가장 중요한 구분이다.

| 구분 | Synchronous I/O (동기 I/O) | Asynchronous I/O (비동기 I/O) |
| :--- | :--- | :--- |
| **핵심 정의** | I/O 작업(특히 **Phase 2: 커널 $\rightarrow$ 유저 버퍼 복사**)이 완료될 때까지 유저 스레드가 관여하거나 대기함 | 유저 스레드는 I/O 요청과 버퍼 포인터만 커널에 던져놓고 즉시 다른 일을 하며, **커널이 Phase 1 + Phase 2(데이터 복사)까지 전부 끝낸 뒤** 완료 통지를 줌 |
| **완료 통지 의미** | **"Ready"** (지금 소켓 버퍼에 데이터가 도착했으니, 네가 직접 `recv()`를 호출해서 복사해 가라) | **"Completed"** (네가 지정한 유저 버퍼에 데이터 복사까지 다 끝내놨으니 바로 패킷 파싱해라) |
| **디자인 패턴** | **Reactor 패턴** (이벤트에 반응하여 유저가 직접 I/O 수행) | **Proactor 패턴** (미리 작업을 걸어두고 완료 결과만 수령) |
| **대표 기술** | Blocking I/O, Non-Blocking I/O, `select`, `poll`, **`epoll` (Linux)**, `kqueue` (macOS/BSD) | **Windows IOCP**, **Linux `io_uring`** |

---

### 3.1 Reactor 패턴 (Synchronous I/O Multiplexing)

`epoll`이나 `kqueue` 기반의 서버가 채택하는 아키텍처다.

```text
[User Thread (Reactor)]                           [Kernel]
  1. epoll_wait()로 여러 소켓 감시 요청 --------> [Phase 1: 패킷 도착 감시]
     (패킷이 올 때까지 대기)                      ... (3번 소켓에 패킷 도착!)
  2. <-------- "3번 소켓 읽기 가능(Readable)!" 통지
  3. 유저 스레드가 직접 read(3, buf) 호출 ------> [Phase 2: 커널 -> 유저 버퍼 복사]
  4. <-------- 복사 완료 후 리턴
  5. OnRecv(buf) 패킷 파싱 및 로직 실행
```

1. **Event Demultiplexer (`epoll_wait`)**: 수많은 소켓 중 현재 I/O가 가능한(Ready) 상태인 소켓들만 추려낸다.
2. **Event Handler**: 깨어난 유저 스레드가 해당 소켓에 대해 직접 `read()` / `write()` 시스템 콜을 호출하여 커널 버퍼와 유저 버퍼 간의 데이터를 복사한다.

---

### 3.2 Proactor 패턴 (Asynchronous I/O)

Windows **IOCP**와 최신 Linux **`io_uring`**이 채택하는 아키텍처다.

```text
[User Thread (Proactor)]                          [Kernel]
  1. WSARecv(sock, userBuf, overlapped) 호출 ---> 요청 등록 후 즉시 반환!
  2. (유저 스레드는 다른 작업 수행 또는           [Phase 1: 패킷 도착 대기]
      GetQueuedCompletionStatus에서 대기)         ... (패킷 도착!)
                                                  [Phase 2: 커널이 직접 userBuf에 복사!]
  3. <-------- "userBuf에 64바이트 복사 완료됨!" 통지 (Completion Queue)
  4. 별도의 recv 호출 없이 즉시 OnRecv(userBuf) 패킷 파싱 실행!
```

1. **Asynchronous Operation Processor (OS 커널)**: 유저가 미리 넘겨준 버퍼(`userBuf`)와 작업 명세서(`OVERLAPPED` 구조체 또는 SQE)를 받아, 패킷 수신부터 유저 버퍼로의 메모리 복사까지 백그라운드에서 모두 수행한다.
2. **Completion Queue & Handler**: 복사가 끝난 작업은 완료 큐(IOCP Queue / CQ)에 들어가며, 대기 중이던 워커 스레드가 이를 꺼내어 **추가적인 읽기 시스템 콜 없이 곧바로 비즈니스 로직(패킷 파싱)을 실행**한다.

---

## 4. Multiplexing(다중화)의 기초와 초기 모델(`select`, `poll`)의 한계

Non-Blocking 소켓을 무식하게 `while` 루프로 폴링(Busy Waiting)하는 문제를 해결하기 위해, **"단 하나의 스레드가 커널에게 여러 개의 소켓 목록을 건네주고, 그중 하나라도 데이터가 도착하면 그때 깨워달라"**고 요청하는 **I/O Multiplexing(다중화)** 기법이 등장했다.

가장 먼저 표준으로 자리 잡은 것이 `select`와 `poll`이지만, 동시 접속자가 수천 명을 넘어가면 급격한 성능 저하가 발생한다. 그 원인은 다음 3가지 구조적 한계 때문이다.

### 4.1 `select` 모델의 동작 원리

`select`는 감시할 소켓들의 파일 디스크립터(FD) 번호를 비트 배열(Bit Array) 형태인 `fd_set` 구조체에 마킹하여 커널에 전달한다.

```cpp
fd_set readSet;
FD_ZERO(&readSet);
for (Socket s : sockets) {
    FD_SET(s, &readSet); // 감시할 소켓 비트 켜기
}

// 커널에 감시 요청 (이벤트 발생할 때까지 블로킹)
int readyCount = select(maxFd + 1, &readSet, nullptr, nullptr, &timeout);

// 리턴 후 어느 소켓에 이벤트가 발생했는지 전수 조사
for (Socket s : sockets) {
    if (FD_ISSET(s, &readSet)) {
        recv(s, ...);
    }
}
```

### 4.2 `select`와 `poll`의 3대 치명적 한계

#### ① 감시 가능한 소켓(FD) 개수의 제한 (`FD_SETSIZE`)
- `select`가 사용하는 `fd_set`은 고정 크기 비트마스크 배열이다. Linux에서는 기본값이 **`FD_SETSIZE = 1024`**로 고정되어 있어, 디스크립터 번호가 1024 이상인 소켓은 아예 `fd_set`에 넣을 수조차 없다(버퍼 오버플로우 발생).
- Windows에서는 `fd_set`이 소켓 핸들의 배열로 구현되어 있어 매크로(`#define FD_SETSIZE 4096`)로 늘릴 수는 있지만, 아래의 2, 3번 문제 때문에 근본적인 해결책이 되지 못한다.
- (`poll`은 `pollfd` 구조체 배열을 동적으로 할당해 전달하므로 개수 제한 문제는 해결했으나, 여전히 2, 3번 문제를 그대로 안고 있다.)

#### ② 매 호출마다 발생하는 유저 $\leftrightarrow$ 커널 간 메모리 복사 비용
- `select`는 커널에 넘겨준 `fd_set` 원본을 커널이 직접 수정(이벤트가 안 온 비트를 0으로 지움)하여 결과를 돌려준다(Destructive).
- 따라서 `select`를 호출할 때마다 매번 `fd_set`을 초기화하고, 수천 개의 소켓 배열을 **User Space에서 Kernel Space로 통째로 복사(`copy_from_user`)**해야 하며, 반환될 때 다시 **Kernel Space에서 User Space로 복사(`copy_to_user`)**해야 한다.
- 감시할 소켓 목록은 거의 변하지 않는데도, 패킷 하나를 읽을 때마다 수천 개의 소켓 등록 정보를 커널에 매번 다시 복사해 넣는 심각한 비효율이 발생한다.

#### ③ $O(N)$ 전수 순회(Linear Scan) 오버헤드 (가장 치명적)
- **커널 내부에서의 $O(N)$ 순회**: 커널은 유저가 넘겨준 배열을 처음부터 끝까지($N$개) 돌면서 각 소켓의 상태를 확인하고 대기 큐에 등록한다. 소켓 1개에 패킷이 도착해 깨어날 때도 또다시 $N$개를 순회하며 비트마스크를 마킹한다.
- **유저 공간에서의 $O(N)$ 순회**: `select`나 `poll`이 `"1개의 소켓에 이벤트가 발생했다"`고 `1`을 반환하더라도, **그게 몇 번 소켓인지는 알려주지 않는다.** 결국 유저 스레드도 `for`문을 돌며 10,000개의 소켓에 대해 일일이 `FD_ISSET()`을 검사해야 한다.

```text
[동시 접속자 N = 10,000명, 그중 단 1명이 패킷을 보낸 상황]

select / poll 방식:
1. 10,000개 소켓 목록을 유저 -> 커널로 복사
2. 커널이 10,000개 소켓을 루프 돌며 검사 (O(N))
3. 유저로 복귀 후, 유저 스레드가 다시 10,000번 루프를 돌며 FD_ISSET 확인 (O(N))
=> 패킷 1개 처리하는 데 수만 번의 루프 실행! (접속자 수 N에 비례하여 서버 성능이 선형적으로 추락)
```

---

## 5. 현대 고성능 I/O 모델로의 전환

`select`와 `poll`의 한계를 분석하면, 동시 접속자 수만 명을 감당하는 고성능 I/O 엔진이 갖추어야 할 조건이 명확해진다.

1. **상태 유지 (Stateful Registration)**: 감시할 소켓 목록을 커널 내부 자료구조(예: Red-Black Tree, IOCP 핸들)에 **최초 1회만 등록**해 두고, 매번 유저-커널 간에 소켓 목록을 복사하지 않아야 한다.
2. **$O(1)$ 이벤트 통지**: 전체 소켓($N$)을 순회하는 대신, NIC 인터럽트(콜백)가 발생했을 때 **실제로 이벤트가 발생한 소켓($K$개)만 완료 큐(Ready List / Completion Queue)에 담아** 유저에게 곧바로 돌려주어야 한다.

이 두 가지 원칙을 완벽하게 구현한 OS별 최종 진화 형태가 바로 다음 장에서 다룰 **Windows의 IOCP**와 **Linux의 `epoll` / `io_uring`**이다.

---

## 요약 체크리스트 (면접 및 실습 핵심 포인트)

1. **Blocking vs Non-Blocking**: 호출 즉시 제어권이 돌아오는가의 차이. Non-Blocking 소켓에서 `EWOULDBLOCK`(`WSAEWOULDBLOCK`)은 에러가 아니라 "현재 커널 버퍼에 데이터가 없음"을 뜻하는 정상 응답이다.
2. **Synchronous vs Asynchronous**: 커널 버퍼에서 유저 버퍼로 **데이터를 복사(Phase 2)하는 주체가 누구인가**의 차이. 유저가 깨어나서 직접 `recv`를 호출해 복사하면 동기(`epoll` - Reactor), 커널이 복사까지 마치고 알려주면 비동기(`IOCP`, `io_uring` - Proactor)다.
3. **`select`의 한계**: ① `FD_SETSIZE`(1024개) 제한, ② 매 호출 시 유저-커널 간 소켓 목록 전체 복사, ③ 커널과 유저 양측에서 발생하는 **$O(N)$ 선형 순회 비용** 때문에 대규모 게임 서버에서는 사용할 수 없다.
