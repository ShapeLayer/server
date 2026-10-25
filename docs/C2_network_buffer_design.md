# Part C-2. 네트워크 버퍼 설계 (RecvBuffer & SendBuffer)

게임 서버의 전체 CPU 사용량 중 상당 부분은 **"바이트 배열을 이리저리 복사(`memcpy`, `memmove`)하고 시스템 콜(`WSASend`, `writev`)을 호출하는 비용"**에서 소모된다.

본 문서에서는 TCP 수신 스트림을 조립할 때 메모리 이동을 최소화하는 **수신 버퍼(`RecvBuffer` / Ring Buffer)** 설계와, 수백 명에게 동시에 패킷을 브로드캐스팅할 때 복사 비용과 시스템 콜 횟수를 극단적으로 줄이는 **송신 버퍼 풀링(`SendBufferChunk`) 및 Scatter-Gather I/O** 기법을 다룬다.

---

## 1. 수신 버퍼 설계: Ring Buffer / Linear Buffer

### 1.1 왜 일반 `char buf[4096]` 배열 하나로는 부족한가?

Part A-1에서 배웠듯 TCP는 패킷 경계가 없는 스트림 프로토콜이므로 **패킷 분할(Fragmentation)**이 수시로 일어난다.
- 예: 클라이언트가 100바이트 패킷을 보냈는데 이번 `WSARecv`에서는 **60바이트만** 도착했다.
- 이 60바이트는 아직 온전한 패킷이 아니므로 파싱할 수 없고, **나머지 40바이트가 다음 `WSARecv`에서 도착할 때까지 세션의 수신 버퍼 어딘가에 안전하게 보관**해 두어야 한다.
- 이때 매번 처리하고 남은 자투리 바이트를 배열 맨 앞(0번 인덱스)으로 당겨오는 `::memmove()`를 매 수신마다 호출하면 불필요한 메모리 복사 비용이 발생한다.

---

### 1.2 `ReadPos`와 `WritePos` 투 포인터 기반 버퍼 (`RecvBuffer`)

메모리 복사를 최소화하기 위해 읽기 위치(**`ReadPos`**)와 쓰기 위치(**`WritePos`**) 두 개의 커서를 두어 관리한다.

```text
[RecvBuffer 메모리 레이아웃]

+-----------------------+-----------------------------------+---------------------------+
| 이미 파싱 완료된 영역  | 아직 조립 중인 유효 데이터 (Data) | 새로 수신 가능한 빈 공간   |
+-----------------------+-----------------------------------+---------------------------+
0                       ^                                   ^                           ^
                        |                                   |                           |
                     _readPos                           _writePos                  _bufferSize
                     |<----------- DataSize() ------------->|<------- FreeSize() ------>|
```

- **`DataSize()` (`_writePos - _readPos`)**: 현재 커널로부터 읽어들였으나 아직 컨텐츠 패킷으로 조립/처리되지 않고 남아있는 유효 바이트 크기.
- **`FreeSize()` (`_bufferSize - _writePos`)**: 다음 `WSARecv` / `read` 호출 시 커널이 데이터를 써넣을 수 있는 여유 공간 크기.

---

### 1.3 두 가지 구현 방식 비교: 고전적 원형 큐(Circular Ring Buffer) vs 여유 공간 선형 버퍼(Linear Buffer)

#### 방식 A: 고전적 원형 링버퍼 (Modulo Wrapping Ring Buffer)
- `_writePos`가 버퍼 끝에 도달하면 `% _bufferSize` 연산을 통해 다시 `0`번 인덱스로 돌아가서(Wrap-around) 빈 공간을 채우는 방식이다.
- **치명적인 단점 (패킷 경계 걸침 현상 - Straddling Packet)**:
  - 만약 100바이트짜리 패킷의 앞쪽 60바이트는 버퍼 맨 끝(`4036 ~ 4095`)에 쓰이고, 뒤쪽 40바이트는 0번 인덱스로 돌아가(`0 ~ 39`) 나뉘어 저장되면, **메모리 주소가 연속적이지 않게 된다!**
  - 이렇게 쪼개진 패킷을 `reinterpret_cast<PacketHeader*>(ptr)`로 캐스팅하거나 Protobuf `ParseFromArray(ptr, size)`에 넘기려면, 결국 **또 다른 연속된 임시 버퍼에 앞쪽 60B와 뒤쪽 40B를 합쳐서 복사(`memcpy`)**해야 하는 배보다 배꼽이 더 큰 상황이 발생한다.

#### 방식 B: 실무 게임 서버의 정석 — 넉넉한 크기의 선형 슬라이딩 버퍼 (`Clean` 최적화)
현대 C++ 게임 서버에서는 패킷이 두 조각으로 나뉘는 현상 자체를 없애기 위해, **버퍼 크기를 최대 패킷 크기의 수 배(예: `4KB * 10 = 40KB`)로 넉넉하게 잡고 다음 두 가지 규칙(`Clean`)으로 커서를 정리**한다.

1. **규칙 1 (가장 빈번한 케이스 - 복사 비용 0!)**:
   - 패킷 파싱 루프(`OnRecv`)가 끝난 직후, 만약 운 좋게 모든 패킷이 깔끔하게 맞아떨어져 **`_readPos == _writePos` (남은 자투리 데이터가 0바이트)**가 되었다면?
   - 단 1바이트의 `memcpy`도 없이, **그냥 `_readPos = 0; _writePos = 0;`으로 두 정수 변수만 0으로 초기화**한다! (실제 게임 트래픽의 95% 이상은 이 케이스에 걸려 복사 비용이 아예 발생하지 않는다!)
2. **규칙 2 (여유 공간 부족 시에만 1회 당기기)**:
   - 자투리 데이터가 조금 남아있더라도 `_BufferSize`가 넉넉하므로 그냥 `_readPos`와 `_writePos`를 계속 오른쪽으로 전진시킨다.
   - 그러다 마침내 남은 여유 공간(`FreeSize()`)이 최대 패킷 1개 크기(`bufferSize` 단위)보다 작아졌을 때만, 그때 딱 한 번 **`::memcpy(&_buffer[0], &_buffer[_readPos], dataSize)`로 자투리 데이터를 맨 앞으로 당겨준다.**

#### `RecvBuffer` 실전 구현 코드
```cpp
class RecvBuffer
{
    // 실제 요청한 버퍼 크기보다 10배 크게 잡아 복사(memcpy) 빈도를 극단적으로 낮춤!
    static constexpr int32_t BUFFER_COUNT = 10;

public:
    RecvBuffer(int32_t bufferSize) : _bufferSize(bufferSize)
    {
        _capacity = bufferSize * BUFFER_COUNT;
        _buffer.resize(_capacity);
    }

    void Clean()
    {
        int32_t dataSize = DataSize();
        if (dataSize == 0)
        {
            // [핵심 최적화] 처리하고 남은 자투리 데이터가 없다면?
            // 메모리 복사 없이 커서만 0으로 리셋! (비용 O(1))
            _readPos = _writePos = 0;
        }
        else
        {
            // 여유 공간이 1개 청크 크기 미만으로 좁아졌을 때만 맨 앞으로 데이터를 당김
            if (FreeSize() < _bufferSize)
            {
                ::memcpy(&_buffer[0], &_buffer[_readPos], dataSize);
                _readPos = 0;
                _writePos = dataSize;
            }
        }
    }

    bool OnRead(int32_t numOfBytes)
    {
        if (numOfBytes > DataSize()) return false;
        _readPos += numOfBytes;
        return true;
    }

    bool OnWrite(int32_t numOfBytes)
    {
        if (numOfBytes > FreeSize()) return false;
        _writePos += numOfBytes;
        return true;
    }

    uint8_t* ReadPos() { return &_buffer[_readPos]; }
    uint8_t* WritePos() { return &_buffer[_writePos]; }
    int32_t DataSize() const { return _writePos - _readPos; }
    int32_t FreeSize() const { return _capacity - _writePos; }

private:
    int32_t _capacity = 0;
    int32_t _bufferSize = 0;
    int32_t _readPos = 0;
    int32_t _writePos = 0;
    std::vector<uint8_t> _buffer;
};
```
> **고급 기법 (Virtual Ring Buffer / Magic Ring Buffer)**: OS의 가상 메모리 매핑 API(`MapViewOfFile3` / `mmap`)를 사용하여 **연속된 두 개의 가상 메모리 페이지 주소가 동일한 하나의 물리 메모리 페이지를 가리키도록 이중 매핑**하면, 원형 큐에서 데이터가 끝단을 넘어가더라도 가상 주소 상으로는 완벽하게 연속된 선형 배열로 보이게 만드는 기법도 존재한다.

---

## 2. 송신 버퍼 설계: SendBuffer Pooling & Scatter-Gather I/O

게임 서버에서 수신(`Recv`)보다 훨씬 까다롭고 부하가 큰 작업이 바로 **송신(`Send`)과 브로드캐스팅(Broadcasting)**이다.

### 2.1 잘못된 송신 설계가 서버를 죽이는 과정 ($O(N^2)$ 복사와 시스템 콜 폭발)

한 방(`GameRoom`)에 100명의 플레이어가 모여 있고, 모두가 초당 10번씩 이동 패킷(50 Byte)을 보낸다고 가정하자.
- 초당 방에 들어오는 이동 패킷 수 = $100 \times 10 = 1,000$개.
- 방에 있는 100명 모두에게 이 이동 사실을 알려주어야(Broadcast) 하므로, 서버가 초당 전송해야 하는 총 패킷 수 = $100\text{명} \times 1,000\text{개} = \mathbf{100,000\text{개/초}}$.

만약 초보자처럼 `Session::Send(char* pkt, int size)` 함수 안에서 **각 세션마다 개별 링버퍼에 패킷을 `memcpy`로 복사해 넣고 그 자리에서 `WSASend()`를 바로 호출**하면 다음 세 가지 재앙이 발생한다:
1. **직렬화 및 메모리 복사 10만 번**: 완전히 똑같은 이동 패킷 내용인데도 100명의 세션 버퍼에 일일이 100번씩 `memcpy`가 일어난다.
2. **`WSASend` 시스템 콜 초당 10만 번 호출**: 유저-커널 전환 비용과 IOCP 완료 통지 패킷 10만 개가 쏟아진다.
3. **비동기 `WSASend` 중첩 호출 문제**: 앞선 `WSASend`가 아직 완료되지 않았는데 또 `WSASend`를 연속해서 걸면, 커널의 Page Locking이 기하급수적으로 늘어나고 멀티스레드 환경에서 패킷 전송 순서가 뒤섞일 위험이 있다.

---

### 2.2 해결책 1: 참조 카운팅(`shared_ptr`) 기반 Zero-Copy 브로드캐스팅과 `SendBufferChunk`

#### ① Zero-Copy 브로드캐스팅의 원리
100명에게 똑같은 이동 패킷을 보낼 때 패킷 직렬화는 **딱 1번만** 수행하여 `SendBuffer` 객체에 담고, 100명의 세션에게는 데이터 복사가 아니라 **`std::shared_ptr<SendBuffer>` 포인터만 공유해서 전달**한다!
- 100명의 세션이 모두 `WSASend` 전송을 완료하여 참조 카운트가 `0`이 되는 그 순간에 버퍼가 자동으로 풀(Pool)에 반납된다.

#### ② TLS 기반 `SendBufferChunk` (거대 메모리 블록 쪼개 쓰기)
그렇다고 50바이트짜리 작은 패킷을 만들 때마다 매번 50바이트짜리 `SendBuffer`를 풀에서 꺼내면 오버헤드가 크다.
따라서 각 스레드의 **TLS(Thread Local Storage)**에 **큰 덩어리 버퍼(`SendBufferChunk`, 예: 64KB)**를 하나 할당해 두고, 패킷을 만들 때마다 마치 김밥을 썰듯 앞으로 전진하며 작은 조각(`SendBuffer`)으로 잘라서 나눠준다!

```text
[TLS SendBufferChunk (64KB 고정 배열, ObjectPool로 관리)]

+-------------------+-----------------------+---------------+---------------------------+
| 패킷 1 (50 Byte)  | 패킷 2 (120 Byte)     | 패킷 3 (40B)  | 아직 남은 여유 공간 (Free)|
+-------------------+-----------------------+---------------+---------------------------+
^                   ^                       ^               ^
SendBuffer A        SendBuffer B            SendBuffer C    _usedSize (다음 할당 위치)
(각 SendBuffer는 자신이 속한 부모 SendBufferChunk의 shared_ptr를 들고 있어 생명주기를 유지함!)
```

1. 워커 스레드는 패킷을 직렬화하기 전에 자신의 TLS에 있는 `LCurrentSendBufferChunk`에서 필요한 크기만큼 공간을 열어달라고 요청(`Open(allocSize)`)한다.
2. 직렬화가 끝나면 실제로 사용한 바이트 크기만큼 확정(`Close(writeSize)`)하고, 해당 위치만 가리키는 가벼운 뷰(View) 객체인 `SendBufferRef`를 반환받는다.
3. 각 `SendBuffer`는 자신이 썰려 나온 모체(`SendBufferChunkRef`)를 멤버로 참조하고 있으므로, 이 청크에서 썰려 나간 수백 개의 패킷들이 모든 세션에서 전송 완료될 때까지 `SendBufferChunk`는 안전하게 살아있다가, 마지막 패킷의 전송이 끝나는 순간 통째로 `ObjectPool`에 반납된다!
4. **효과**: 스레드 간 락 경합 0회, 메모리 할당 비용 0회, 브로드캐스팅 시 패킷 메모리 복사 0회 달성!

---

### 2.3 해결책 2: 1-Pending Send 제한과 Scatter-Gather I/O (`WSABUF` 배열 전송)

`Session::Send(SendBufferRef buf)`가 호출될 때마다 무작정 `WSASend` 시스템 콜을 호출하지 않고, **세션 내부의 전송 대기 큐(`_sendQueue`)에 차곡차곡 모아서 한 번의 `WSASend`로 뭉쳐 보내는 기법**이다.

#### ① 1-Pending Send (단일 비동기 송신 유지) 패턴
세션마다 현재 커널에 비동기 송신이 걸려있는지 나타내는 플래그(`std::atomic<bool> _sendRegistered`)를 둔다.

```text
[Session::Send(pkt) 호출 흐름]

Thread A: Send(패킷1) -> _sendQueue에 삽입 -> _sendRegistered가 false네? -> 내가 true로 바꾸고 WSASend 호출!
Thread B: Send(패킷2) -> _sendQueue에 삽입 -> 이미 _sendRegistered가 true네! -> 큐에만 넣고 즉시 퇴근!
Thread C: Send(패킷3) -> _sendQueue에 삽입 -> 이미 _sendRegistered가 true네! -> 큐에만 넣고 즉시 퇴근!
   ...
(잠시 후 커널에서 패킷1에 대한 WSASend 완료 통지 도착!)
IOCP Worker: 어? 완료 처리 도중 _sendQueue를 보니 패킷2, 패킷3이 쌓여있네!
             -> 패킷2, 패킷3을 한꺼번에 묶어서(Scatter-Gather) 단 1번의 WSASend로 전송!
```

#### ② Scatter-Gather I/O (Vectored I/O: `WSASend` 다중 버퍼 전송 / Linux `writev`)
위 상황에서 큐에 쌓여있는 `패킷2`(주소 `0x1000`)와 `패킷3`(주소 `0x5000`)은 서로 메모리 주소가 떨어져 있다. 이를 한 번의 시스템 콜로 보내기 위해 또 하나의 큰 버퍼에 `패킷2 + 패킷3`을 `memcpy`로 합쳐야 할까? **전혀 그럴 필요가 없다!**

운영체제의 **`WSASend`**(Windows)와 **`writev` / `sendmsg`**(Linux)는 떨어져 있는 여러 개의 메모리 버퍼 포인터 배열(`WSABUF[]` / `struct iovec[]`)을 넘기면, **OS 커널(또는 NIC의 Gather DMA 하드웨어)이 알아서 순서대로 긁어모아(Gather) 하나의 연속된 TCP 스트림으로 전송**해 주는 **Scatter-Gather I/O** 기능을 기본 제공한다!

```cpp
void Session::Send(SendBufferRef sendBuffer)
{
    bool registerSend = false;
    {
        // 1. 먼저 세션의 송신 대기 큐에 패킷(shared_ptr)을 넣는다. (데이터 복사 없음!)
        WRITE_LOCK;
        _sendQueue.push(sendBuffer);

        // 2. 현재 진행 중인 WSASend가 없을 때만 내가 총대를 메고 등록 권한을 얻는다.
        if (_sendRegistered.exchange(true) == false)
            registerSend = true;
    }

    if (registerSend)
        RegisterSend();
}

void Session::RegisterSend()
{
    _sendEvent.Init();
    _sendEvent.owner = shared_from_this(); // 세션 생명주기 연장

    // 3. 큐에 쌓여있는 모든 SendBuffer들을 꺼내어 sendEvent에 옮겨 담는다.
    {
        WRITE_LOCK;
        int32_t writeSize = 0;
        while (!_sendQueue.empty())
        {
            SendBufferRef sendBuffer = _sendQueue.front();
            writeSize += sendBuffer->WriteSize();
            // (실무에서는 너무 많이 쌓였을 때 한 번에 보내는 최대 바이트 상한선을 둠)

            _sendQueue.pop();
            // WSASend가 완료될 때까지 SendBuffer(및 Chunk)가 파괴되지 않도록 참조 유지!
            _sendEvent.sendBuffers.push_back(sendBuffer);
        }
    }

    // 4. Scatter-Gather I/O: 여러 개의 떨어진 버퍼들을 WSABUF 배열로 묶어서 구성
    std::vector<WSABUF> wsaBufs;
    wsaBufs.reserve(_sendEvent.sendBuffers.size());
    for (SendBufferRef& sb : _sendEvent.sendBuffers)
    {
        WSABUF wsaBuf;
        wsaBuf.buf = reinterpret_cast<char*>(sb->Buffer());
        wsaBuf.len = static_cast<ULONG>(sb->WriteSize());
        wsaBufs.push_back(wsaBuf);
    }

    // 5. 단 1번의 WSASend 시스템 콜로 수십 개의 패킷을 한꺼번에 커널로 밀어 넣음!
    DWORD numOfBytes = 0;
    if (SOCKET_ERROR == ::WSASend(_socket, wsaBufs.data(), static_cast<DWORD>(wsaBufs.size()), 
                                  &numOfBytes, 0, &_sendEvent, nullptr))
    {
        int32_t errorCode = ::WSAGetLastError();
        if (errorCode != WSA_IO_PENDING)
        {
            HandleError(errorCode);
            _sendEvent.owner = nullptr;
            _sendEvent.sendBuffers.clear();
            _sendRegistered.store(false);
        }
    }
}
```
> **추가 최적화 (1-Tick Send Flush)**: 아예 `Session::Send()`에서는 큐에 넣기만 하고 `RegisterSend()`를 호출하지 않은 뒤, 서버 루프에서 일정 주기(예: 10ms~16ms마다 한 번)로 모든 세션의 큐를 한꺼번에 Flush(`RegisterSend`)하도록 만들면 시스템 콜 횟수를 극한까지 줄일 수 있다.

---

## 요약 체크리스트 (면접 및 실습 핵심 포인트)

1. **`RecvBuffer` 설계**: TCP 스트림 파싱 시 매번 `memmove`를 호출하는 비용을 없애기 위해 `ReadPos`와 `WritePos` 두 커서를 사용한다. 버퍼 크기를 넉넉히 잡고 **`ReadPos == WritePos`일 때는 복사 없이 두 커서만 `0`으로 초기화**하며, 여유 공간이 부족할 때만 데이터를 앞으로 당긴다. (고전적 원형 큐는 패킷이 버퍼 끝과 처음에 쪼개져 걸치는 문제 때문에 선형 슬라이딩 버퍼나 가상 메모리 이중 매핑 기법을 선호한다.)
2. **`SendBufferChunk`와 Zero-Copy 브로드캐스팅**: 스레드별 **TLS에 거대한 `SendBufferChunk`**를 두고 잘라서 쓰며, 다수에게 브로드캐스팅할 때는 패킷을 1번만 직렬화한 뒤 **`shared_ptr<SendBuffer>` 참조만 공유**하여 복사 비용을 0으로 만든다.
3. **Scatter-Gather I/O와 1-Pending Send**: 패킷마다 `WSASend`를 호출하지 않고 세션당 **한 번에 1개의 `WSASend`만 걸리도록(`_sendRegistered`) 제어**하며, 그사이에 큐에 쌓인 여러 패킷들은 **`WSABUF` 배열(Linux는 `writev`의 `iovec`)로 묶어 단 한 번의 시스템 콜로 모아서 전송**한다.
