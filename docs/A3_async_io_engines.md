# Part A-3. 고성능 OS 비동기/멀티플렉싱 I/O 엔진

수만 개의 동시 연결을 $O(1)$의 이벤트 감지 비용과 최소한의 컨텍스트 스위칭(Context Switching)으로 처리하기 위해, Windows는 **IOCP(I/O Completion Port)**를, Linux는 **`epoll`**과 최신 비동기 인터페이스인 **`io_uring`**을 제공한다.

국내 PC/모바일 MMORPG 서버의 주력인 Windows IOCP의 내부 커널 아키텍처와, 글로벌/클라우드 환경의 표준인 Linux `epoll` 및 `io_uring`의 핵심 동작 원리를 심층 분석한다.

---

## 1. Windows IOCP (I/O Completion Port)

IOCP는 Windows NT 커널이 제공하는 **진정한 의미의 비동기 I/O(Asynchronous I/O, Proactor 패턴)이자 고도로 최적화된 커널 레벨 스레드 풀링(Thread Pooling) 엔진**이다.

### 1.1 Overlapped I/O와 비동기 송수신 (`WSASend`, `WSARecv`)

Windows에서 비동기 소켓 I/O를 수행하려면 반드시 `WSA_FLAG_OVERLAPPED` 플래그로 소켓을 생성해야 하며, 모든 I/O 요청 시 커널과 유저가 공유하는 작업 명세서인 **`WSAOVERLAPPED`** 구조체와 유저 버퍼 포인터(`WSABUF`)를 함께 넘겨야 한다.

#### 핵심 주의사항: `OVERLAPPED` 구조체와 버퍼의 생명주기(Life-cycle)
`WSARecv()`나 `WSASend()`가 반환(`SOCKET_ERROR` 및 `WSA_IO_PENDING`)되었다는 것은 **"작업이 완료되었다"**는 뜻이 아니라 **"커널에 비동기 작업이 성공적으로 접수되었다"**는 뜻이다.
따라서 커널이 백그라운드에서 데이터를 복사하고 IOCP 완료 큐를 통해 통지(`GetQueuedCompletionStatus`)를 줄 때까지 다음 두 가지는 **절대 메모리에서 해제되거나 재사용되어서는 안 된다**:
1. **`WSAOVERLAPPED` 구조체**: 지역 변수(Stack)로 선언하면 함수 종료와 함께 파괴되어 커널이 엉뚱한 스택 메모리를 덮어쓰는 참사(Stack Corruption)가 발생한다. 반드시 힙(Heap)이나 Object Pool에 할당해야 한다.
2. **`WSABUF`가 가리키는 실제 데이터 버퍼(`RecvBuffer`, `SendBuffer`)**: 커널이 언제든 이 주소에 직접 데이터를 쓰거나 읽어갈 수 있으므로 I/O 완료 통지 전까지 세션이나 버퍼가 삭제되지 않도록 **참조 카운팅(`std::shared_ptr` 등)**으로 생명을 연장해야 한다.

#### `OVERLAPPED` 구조체 확장 패턴 (Object Slicing이 아닌 메모리 배치 활용)
완료 통지 함수인 `GetQueuedCompletionStatus(GQCS)`는 완료된 작업의 `LPOVERLAPPED` 포인터를 돌려준다. 이때 이 작업이 `Accept`인지, `Recv`인지, `Send`인지 구분하고 관련 데이터를 찾기 위해 C++에서는 **`WSAOVERLAPPED`를 첫 번째 멤버로 두거나 상속받는 확장 구조체**를 사용한다.

```cpp
enum class IoType : uint8_t { Accept, Recv, Send };

// WSAOVERLAPPED를 상속받아 첫 번째 메모리 오프셋(0번지)에 위치시킴
struct OverlappedEx : public WSAOVERLAPPED
{
    IoType type;
    std::shared_ptr<Session> owner; // I/O 진행 중 Session이 파괴되지 않도록 강한 참조 유지!
    std::vector<WSABUF> wsaBufs;

    OverlappedEx(IoType ioType) : type(ioType)
    {
        // 부모인 WSAOVERLAPPED 영역(Internal, InternalHigh, Offset, hEvent)만 0으로 초기화
        ::memset(static_cast<WSAOVERLAPPED*>(this), 0, sizeof(WSAOVERLAPPED));
    }
};

// GQCS에서 꺼낼 때:
WSAOVERLAPPED* overlapped = nullptr;
DWORD bytesTransferred = 0;
ULONG_PTR completionKey = 0;

if (::GetQueuedCompletionStatus(iocpHandle, &bytesTransferred, &completionKey, &overlapped, INFINITE))
{
    // 메모리 시작 주소가 동일하므로 안전하게 다운캐스팅 가능
    OverlappedEx* overlappedEx = static_cast<OverlappedEx*>(overlapped);
    overlappedEx->owner->Dispatch(overlappedEx, bytesTransferred);
}
```

---

### 1.2 IOCP 커널 내부 구조와 스레드 풀링 메커니즘

IOCP가 단순한 비동기 I/O를 넘어 최고의 성능을 내는 이유는 **커널 내부에서 워커 스레드의 깨어남과 잠듦을 직접 제어하여 Context Switching을 극단적으로 억제**하기 때문이다.

IOCP 커널 객체 내부에는 크게 3개의 자료구조가 존재한다:

```text
+-------------------------------------------------------------------------+
|                           IOCP Kernel Object                            |
|                                                                         |
|  1. Device List (연결된 소켓들과 Completion Key 목록)                   |
|                                                                         |
|  2. I/O Completion Queue (FIFO)                                         |
|     [ 완료 패킷 1 ] -> [ 완료 패킷 2 ] -> [ 완료 패킷 3 ]               |
|     (WSARecv/WSASend 완료 또는 PostQueuedCompletionStatus로 삽입됨)     |
|                                                                         |
|  3. Waiting Thread Queue (LIFO - 스택 구조!)                            |
|     [ Thread C (가장 최근 대기) ] -> [ Thread B ] -> [ Thread A ]       |
|                                                                         |
|  * Concurrency Value (목표 동시 실행 스레드 수, 예: 8코어)              |
|  * Active(Running) Thread Count (현재 깨어서 일하는 스레드 수)          |
+-------------------------------------------------------------------------+
```

#### ① 왜 Waiting Thread Queue는 FIFO가 아니라 LIFO(후입선출)인가?
- `GetQueuedCompletionStatus(GQCS)`를 호출해 대기 상태로 들어간 스레드들은 **LIFO(Stack)** 순서로 관리된다.
- 즉, **가장 최근에 일을 마치고 방금 잠든 스레드가 다음 완료 통지 때 가장 먼저 다시 깨어난다.**
- **이유 (CPU 캐시 친화성 & 메모리 절약)**: 방금까지 실행되던 스레드는 해당 스레드의 스택 메모리와 문맥 정보가 아직 **CPU의 L1/L2/L3 캐시에 따뜻하게 남아있을(Cache Warm) 확률이 가장 높다.** 또한 오랫동안 깨어나지 않는 하위 스레드들의 스택 메모리는 OS가 디스크(Page File)로 스왑 아웃(Swap-out)시켜 물리 RAM을 절약할 수 있다.

#### ② Concurrency(동시성 수치)와 Context Switching 제어 원리
`CreateIoCompletionPort()` 호출 시 마지막 인자인 `NumberOfConcurrentThreads`를 설정한다(보통 `0`을 넣으면 OS가 CPU 논리 코어 수로 자동 설정한다).

- **가정**: CPU 코어는 8개(`Concurrency = 8`)이고, `GQCS`를 호출해 둔 전체 워커 스레드 풀의 크기는 16개인 상황.
- **동작 규칙**:
  1. I/O 완료 큐(Completion Queue)에 완료 통지가 아무리 많이 쌓여 있어도, 현재 깨어나서 코드를 실행 중인 **Active Thread 수가 `Concurrency(8)` 이상이면 커널은 대기 중인 나머지 8개 스레드를 절대 깨우지 않는다.**
  2. 코어 수(8개)만큼의 스레드가 이미 풀가동 중인데 대기 스레드를 더 깨워봤자 코어를 뺏기 위한 **Context Switching만 발생할 뿐 처리량은 늘어나지 않기 때문**이다.
  3. **예외 상황 (블로킹 감지 및 자동 보충)**: 만약 실행 중이던 8개의 스레드 중 하나가 내부에서 `Mutex` 대기, `Sleep()`, 동기 파일/DB I/O 등으로 인해 **커널 대기 상태(Blocked)로 빠지면**, IOCP 커널은 즉시 `Active Thread Count`를 1 감소(`8 -> 7`)시키고, 놀고 있는 코어를 채우기 위해 **Waiting Thread Queue에서 대기 중이던 9번째 스레드를 즉시 깨워 투입**한다.
  4. 이후 블로킹되었던 스레드가 깨어나면 일시적으로 Active Thread가 9개가 되지만, 스레드 중 하나가 일을 마치고 다시 `GQCS`를 호출하는 순간 커널이 그 스레드를 재워서 다시 8개로 맞춘다.

---

### 1.3 Page Locking 한계와 Zero-Byte Receive 기법

#### ① Page Locking (Non-Paged Pool) 문제란?
`WSARecv`나 `WSASend`를 호출할 때 유저 공간의 버퍼를 커널에 넘기면, 네트워크 드라이버(DMA)나 커널이 그 메모리에 언제든 접근할 수 있어야 한다. 만약 그사이에 OS의 가상 메모리 관리자가 해당 유저 버퍼가 속한 메모리 페이지(Page, 4KB 단위)를 디스크(Page File)로 스왑 아웃시켜 버리면 커널 패닉(BSOD, 블루스크린)이 발생한다.

따라서 Windows 커널은 **비동기 I/O가 진행 중인(Pending) 모든 유저 버퍼의 메모리 페이지를 물리 RAM에서 절대 쫓겨나지 않도록 강제로 잠근다(Page Locking)**.
- OS에는 이렇게 잠글 수 있는 물리 메모리의 상한선(Non-Paged Pool 한계)이 정해져 있다.
- 만약 동시 접속자 50,000명이 접속한 서버에서 각 세션마다 64KB짜리 수신 버퍼로 `WSARecv`를 걸어두면, 아무도 패킷을 보내지 않아도 무려 **50,000개의 페이지가 물리 메모리에 강제 잠금**되며, 한계를 초과하는 순간 `WSAENOBUFS` (에러 코드 10055)가 발생하며 서버의 모든 네트워크 기능이 마비된다.

#### ② 해결책: Zero-Byte Receive (0바이트 수신 기법)
이 문제를 우회하기 위해 고안된 기법이 **Zero-Byte Receive**다.

```cpp
// 1단계: 길이를 0으로 설정하여 WSARecv 요청 (메모리 페이지가 잠기지 않음!)
WSABUF wsaBuf;
wsaBuf.buf = nullptr;
wsaBuf.len = 0;
::WSARecv(socket, &wsaBuf, 1, &bytesRecv, &flags, &overlapped, nullptr);
```

1. 평소 대기 상태일 때는 버퍼 크기를 **`0`**(`len = 0, buf = nullptr`)으로 설정하여 `WSARecv`를 걸어둔다. 크기가 0이므로 **잠기는 메모리 페이지(Page Lock)가 0개**다.
2. 클라이언트가 실제로 패킷을 보내어 커널 수신 버퍼(`Socket Recv Buffer`)에 데이터가 도착하면, IOCP는 `0바이트 수신 완료` 통지를 깨운다.
3. 깨어난 워커 스레드는 이제 커널 버퍼에 데이터가 이미 들어와 있음을 확실히 알았으므로, Non-blocking 모드로 실제 버퍼를 넘겨 데이터를 단숨에 퍼 올리거나 정상적인 `WSARecv`를 수행한다.
- **장단점**: 5만 명 이상의 유휴(Idle) 세션을 유지할 때 커널의 Non-Paged Pool 고갈을 완벽히 막아주지만, 패킷 수신 시 시스템 콜이 2단계로 나뉘어 미세한 오버헤드가 추가된다. 따라서 연결 수가 적당하고 패킷 송수신이 잦은 일반 게임 서버에서는 세션당 1개의 고정된 작은 Recv 페이지만 잠그고, 대규모 게이트웨이/채팅 서버에서 주로 활용한다.

---

## 2. Linux `epoll`과 `io_uring`

Linux는 전통적으로 `epoll`을 통한 **Reactor(Synchronous Non-blocking Multiplexing)** 모델을 사용해 왔으며, Linux 커널 5.1부터는 IOCP를 뛰어넘는 완전 비동기 모델인 **`io_uring`**을 도입했다.

### 2.1 `epoll`의 커널 내부 구조 ($O(1)$ 감지의 비밀)

`select`/`poll`이 매번 소켓 목록을 복사하고 $O(N)$으로 순회했던 문제를 해결하기 위해, `epoll`은 커널 내부에 **2개의 핵심 자료구조**를 유지한다.

```text
+-------------------------------------------------------------------------+
|                       eventpoll (Kernel Context)                        |
|                                                                         |
|  1. Red-Black Tree (rbr: 감시 대상 전체 소켓 관리)                      |
|         [fd: 10]                                                        |
|         /      \        <--- epoll_ctl(ADD / MOD / DEL)                 |
|     [fd: 5]  [fd: 23]        검색/삽입/삭제 복잡도: O(log N)            |
|                                                                         |
|  2. Ready List (rdllist: 이벤트가 발생한 소켓만 모은 이중 연결 리스트)  |
|     [fd: 23] <---> [fd: 105]                                            |
|        ^                ^                                               |
|        |----------------|--- NIC 패킷 도착 시 커널 콜백(ep_poll_callback)|
|                              이 O(1)로 Ready List에 즉시 추가!          |
+-------------------------------------------------------------------------+
                                   |
                                   v  epoll_wait() 호출 시
                    Ready List에 있는 소켓들만 유저에게 O(1)로 반환!
```

1. **`epoll_create()`**: 커널 공간에 `eventpoll` 객체를 생성한다. 이 안에는 빈 **Red-Black Tree(자가 균형 이진 탐색 트리)**와 빈 **Ready List(Doubly Linked List)**가 만들어진다.
2. **`epoll_ctl()`**: 감시할 소켓을 추가(`EPOLL_CTL_ADD`), 수정(`MOD`), 삭제(`DEL`)한다. Red-Black Tree를 사용하므로 수만 개의 소켓이 있어도 **$O(\log N)$** 만에 중복 검사 및 등록이 끝나며, 등록할 때 해당 소켓의 커널 대기 큐에 콜백 함수(`ep_poll_callback`)를 걸어둔다. (매번 전체 배열을 복사할 필요가 없다!)
3. **패킷 도착 시 (인터럽트 콜백)**: 네트워크 카드로 패킷이 들어와 특정 소켓 버퍼에 데이터가 차면, 커널은 전체 소켓을 뒤지는 것이 아니라 등록해 둔 콜백(`ep_poll_callback`)을 실행해 해당 소켓 노드를 **Ready List에 $O(1)$로 끼워 넣는다.**
4. **`epoll_wait()`**: 유저 스레드는 단지 **Ready List가 비어있는지 확인($O(1)$)**하고, 들어있는 소켓들만 유저 배열로 복사해 온다.

---

### 2.2 Level-Triggered (LT) vs Edge-Triggered (ET)

`epoll`이 이벤트를 통지하는 방식은 **LT(기본값)**와 **ET(`EPOLLET` 플래그)** 두 가지로 나뉜다. 전기 공학의 신호 파형에서 유래한 용어로, 게임 서버 면접에서 가장 빈번하게 출제되는 주제다.

```text
커널 수신 버퍼 상태:
  데이터 있음 (1) |       +-----------------------+
                 |       |                       |
  데이터 없음 (0) +-------+                       +--------
                         ^                       ^
                         |                       |
                    [Edge(변화점)]          [Edge(변화점)]
                    |<--- Level (높은 상태 유지) --->|
```

| 구분 | Level-Triggered (LT, 기본값) | Edge-Triggered (ET, `EPOLLET`) |
| :--- | :--- | :--- |
| **통지 조건** | 커널 수신 버퍼에 **데이터가 남아있는 동안(Level > 0)** `epoll_wait`를 호출할 때마다 **계속해서** 이벤트를 발생시킴 | 커널 수신 버퍼의 상태가 바뀌는 순간(데이터 없음 $\rightarrow$ 새 데이터 도착, **Edge**)에 **딱 한 번만** 이벤트를 발생시킴 |
| **일부만 읽었을 때** | 버퍼에 100B가 왔는데 40B만 `read()` 하고 나가도, 남은 60B 때문에 다음 `epoll_wait`에서 **즉시 다시 깨어남** | 100B 중 40B만 읽고 나가면, 상대방이 **새로운 패킷을 추가로 보내기 전까지는 남은 60B에 대해 영원히 다시 알려주지 않음!** |
| **소켓 모드** | Blocking / Non-Blocking 모두 가능 | **반드시 Non-Blocking 소켓 + `EAGAIN` 루프 필수** |
| **장단점** | 구현이 안전하고 직관적이나, 불필요한 `epoll_wait` 깨어남 및 시스템 콜이 더 발생할 수 있음 | 불필요한 이벤트 통지 횟수를 최소화하고 멀티스레드 환경에서 `EPOLLONESHOT`과 결합해 고성능을 내기 좋으나, 구현 실수 시 패킷이 멈추는 버그 발생 |

---

### 2.3 왜 Edge-Triggered (ET) 모드에서는 Non-blocking 루프가 필수인가?

ET 모드의 규칙은 명확하다: **"한 번 깨어났을 때 커널 버퍼의 바닥이 보일 때까지(다 비울 때까지) 남김없이 읽어야 한다."**

만약 ET 모드에서 한 번의 이벤트에 `read()`를 딱 1번만 호출하거나 Blocking 소켓을 쓰면 다음 두 가지 재앙이 발생한다:

1. **한 번만 `read()` 할 경우 (패킷 멈춤 / 기아 현상)**:
   - 클라이언트가 200B짜리 로그인 요청 패킷을 보냈는데, 서버가 `read(128)`로 128B만 읽고 루프를 끝냈다.
   - 서버 패킷 파서는 나머지 72B가 아직 안 읽혔으므로 로그인 처리를 보류한다.
   - 클라이언트는 로그인 응답이 올 때까지 아무 패킷도 추가로 보내지 않고 기다린다.
   - 그러나 `epoll` ET 모드는 **새 패킷이 도착하지 않으므로** 커널 버퍼에 남아있는 72B에 대해 절대 이벤트를 다시 주지 않는다. 결국 클라이언트와 서버가 서로 영원히 기다리는 **데드락(Hang)**에 빠진다.
2. **Blocking 소켓으로 끝까지 읽으려 할 경우 (스레드 멈춤)**:
   - 그렇다고 `while(true)` 루프로 계속 `read()`를 호출하는데 소켓이 **Blocking 모드**라면?
   - 데이터를 다 읽고 버퍼가 비워진 직후 호출된 마지막 `read()`에서 **워커 스레드 전체가 블로킹(멈춤)**되어 버린다.

#### ET 모드의 정석 구현 코드 (`while` + Non-blocking + `EAGAIN`)
따라서 ET 모드에서는 소켓을 반드시 **Non-blocking**으로 만들고, `read()`가 **`EAGAIN` (`EWOULDBLOCK`)**을 반환할 때까지 `while` 루프를 돌려야 한다.

```cpp
// epoll ET 모드 수신 처리 정석 패턴
void OnEpollReadEvent(int clientFd)
{
    while (true)
    {
        // RecvBuffer의 남은 쓰기 공간에 데이터를 읽어 들임
        ssize_t bytesRead = ::read(clientFd, recvBuffer.WritePos(), recvBuffer.FreeSize());

        if (bytesRead > 0)
        {
            recvBuffer.OnWrite(bytesRead);
            // 만약 유저의 RecvBuffer가 가득 찼다면 먼저 패킷 파싱으로 비워줌
            ProcessPackets();
        }
        else if (bytesRead == 0)
        {
            // 상대방이 FIN 전송 (연결 종료)
            Disconnect(clientFd);
            break;
        }
        else // bytesRead == -1
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                // [핵심] 커널 수신 버퍼의 모든 데이터를 남김없이 다 읽었음!
                // 이제 안심하고 루프를 빠져나가 다음 epoll 이벤트를 기다리면 됨.
                break;
            }
            else if (errno == EINTR)
            {
                // 시그널에 의해 중단됨, 다시 시도
                continue;
            }
            else
            {
                // 진짜 소켓 에러 발생
                Disconnect(clientFd);
                break;
            }
        }
    }
}
```
> **멀티스레드 주의사항 (`EPOLLONESHOT`)**: 여러 워커 스레드가 하나의 `epoll` 인스턴스를 공유할 때, ET 모드라도 스레드 A가 첫 번째 패킷을 읽는 도중에 새로운 두 번째 패킷이 도착하면 또 Edge가 발생하여 스레드 B가 동시에 같은 소켓을 읽으러 들어오는 레이스 컨디션이 생긴다. 이를 막기 위해 멀티스레드 `epoll` 서버에서는 **`EPOLLONESHOT`** 플래그를 함께 걸어 한 번에 하나의 스레드만 해당 소켓을 처리하도록 보장한다.

---

### 2.4 차세대 Linux 비동기 엔진: `io_uring`

`epoll`은 훌륭하지만 여전히 **Reactor(동기 멀티플렉싱)** 모델이다. 즉, `epoll_wait`로 깨어난 뒤 유저가 직접 `read()` / `write()` 시스템 콜을 호출해야 하므로, 패킷이 많을수록 **User Mode $\leftrightarrow$ Kernel Mode 전환(System Call 오버헤드, Meltdown/Spectre 보안 패치 이후 더욱 비싸짐)**이 빈번하게 발생한다.

이를 해결하기 위해 2019년 Linux 5.1 커널에 도입된 **`io_uring`**은 Windows IOCP와 같은 **Proactor(진정한 비동기)** 모델을 한 단계 더 발전시켜 **시스템 콜 자체를 없애는(Zero-Syscall)** 수준에 도달했다.

```text
      [User Space]                                   [Kernel Space]
           |                                                |
           +---------> +------------------------+ <---------+
   유저가 작업(SQE)을   | Submission Queue (SQ)  |   커널이 SQ에서 작업을 꺼내
   직접 링버퍼에 기록   |  (Shared Ring Buffer)  |   비동기 실행 및 메모리 복사
                       +------------------------+
                                                          |
                       +------------------------+ <-------+
   유저가 완료(CQE)를   | Completion Queue (CQ)  |   커널이 완료 결과를
   직접 링버퍼에서 읽음 |  (Shared Ring Buffer)  |   CQ 링버퍼에 직접 기록
           +---------- +------------------------+
           |
   (User와 Kernel이 mmap으로 메모리 공유 -> 데이터 복사 및 시스템 콜 최소화!)
```

- **동작 원리**:
  1. 유저 공간과 커널 공간이 `mmap`을 통해 **두 개의 링버퍼(Submission Queue, Completion Queue)**를 공유한다.
  2. 유저는 `read`/`write` 요청(SQE)을 공유 메모리의 **SQ**에 적어 넣고, 커널은 완료된 결과(CQE)를 공유 메모리의 **CQ**에 적어 넣는다.
  3. 심지어 커널 폴링 모드(`IORING_SETUP_SQPOLL`)를 켜면, 커널 스레드가 알아서 SQ를 감시하여 가져가므로 **유저 프로세스는 단 한 번의 시스템 콜(`io_uring_enter`)도 호출하지 않고 순수 메모리 읽기/쓰기만으로 네트워크 송수신을 처리**할 수 있다.

---

## 3. Windows IOCP vs Linux `epoll` 총정리 비교

| 비교 항목 | Windows IOCP | Linux `epoll` |
| :--- | :--- | :--- |
| **I/O 모델** | **Asynchronous I/O (Proactor 패턴)** | **Synchronous Multiplexing (Reactor 패턴)** |
| **데이터 복사 주체** | **OS 커널** (유저 버퍼에 복사 완료 후 통지) | **유저 스레드** (통지받은 후 직접 `read`/`write` 호출) |
| **스레드 제어** | 커널이 직접 워커 스레드 풀을 관리 (LIFO 대기 큐 + Concurrency 제한으로 Context Switch 억제) | 스레드 풀 관리 기능 없음 (유저가 직접 스레드를 만들고 `EPOLLONESHOT` 등으로 분배해야 함) |
| **메모리 관리 이슈** | Pending된 버퍼만큼 물리 메모리가 잠김(**Page Locking**). 필요 시 Zero-byte Recv 사용 | 이벤트 감지만 하므로 대기 중 잠기는 유저 버퍼가 없음 (메모리 효율적) |
| **구현 난이도** | 비동기 작업 중 객체가 파괴되지 않도록 **생명주기(참조 카운트) 및 `OVERLAPPED` 관리**가 까다로움 | 동기적으로 `read`를 호출하므로 생명주기 관리가 상대적으로 직관적이나, **ET 모드의 루프 처리** 주의 필요 |

---

## 요약 체크리스트 (면접 및 실습 핵심 포인트)

1. **IOCP의 스레드 스케줄링**: 대기 스레드 큐를 **LIFO(스택)**로 관리하여 CPU 캐시 적중률(Cache Warmth)을 높이고, 실행 중인 스레드 수를 코어 수(`Concurrency`)로 제한하되 실행 스레드가 블로킹되면 대기 스레드를 자동으로 깨워 Context Switching을 최소화한다.
2. **IOCP 생명주기**: `WSARecv`/`WSASend` 호출 후 완료 통지가 올 때까지 `OVERLAPPED` 구조체와 데이터 버퍼가 절대 해제되면 안 되며, 대량 접속 시 Non-Paged Pool 고갈을 막기 위해 **Zero-Byte Receive**를 활용할 수 있다.
3. **`epoll`의 구조와 ET 모드**: `epoll`은 **Red-Black Tree**(감시 등록 $O(\log N)$)와 **Ready List**(이벤트 반환 $O(1)$)로 동작한다. **Edge-Triggered(ET)** 모드는 상태 변화 시 단 한 번만 통지하므로 반드시 **Non-Blocking 소켓에서 `EAGAIN`이 나올 때까지 루프**를 돌아야 한다.
