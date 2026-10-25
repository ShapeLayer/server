# Part C-1. 커스텀 메모리 할당자 (Custom Memory Allocators)

24시간 365일 무중단으로 돌아가는 C++ 게임 서버에서 패킷 객체, `Job` 일감, 투사체, 몬스터 등을 생성하고 파괴할 때마다 기본 연산자인 `new` / `delete` (`malloc` / `free`)를 그대로 호출하면 서버는 두 가지 치명적인 문제(힙 파편화와 멀티스레드 힙 락 경합)에 직면한다.

본 문서에서는 표준 힙 할당의 한계를 분석하고, 실무 게임 서버 엔진이 구축하는 **Object Pool**, **Slab/Size-class 기반 Memory Pool**, **TLS(Thread Local Storage) 캐시 풀**, 그리고 메모리 침범 버그를 즉시 잡아내는 **Stomp Allocator**까지 상세히 다룬다.

---

## 1. 왜 기본 `new` / `delete`를 그대로 쓰면 안 되는가?

### 1.1 힙 파편화 (Heap Fragmentation)
서버가 오랫동안 구동되면서 크기가 제각각인 객체(예: 16B 패킷, 128B 유저 정보, 40B 스킬 객체)를 수억 번 할당하고 해제하면, 물리/가상 메모리 공간이 체스판처럼 구멍이 숭숭 뚫린 상태(**외부 단편화, External Fragmentation**)가 된다.

```text
[장기 구동 후 힙 메모리 상태 (외부 단편화)]
+--------+-------+--------+-------+--------+-------+
| 사용중 | 빈공간| 사용중 | 빈공간| 사용중 | 빈공간|
| (32B)  | (16B) | (64B)  | (24B) | (32B)  | (16B) |
+--------+-------+--------+-------+--------+-------+
 -> 남은 빈 공간의 총합은 56B이지만, 연속된 공간이 없어서 32B 객체 1개도 할당할 수 없음!
```

1. **메모리 부족(OOM) 및 페이지 폴트 증가**: 전체 여유 메모리는 충분한데도 연속된 공간이 없어 OS로부터 새로운 메모리 페이지를 계속 할당받아야 하므로 서버의 전체 메모리 점유율(Working Set)이 끝없이 우상향한다.
2. **CPU 캐시 지역성(Cache Locality) 파괴**: 서로 연관된 객체들이 메모리 곳곳에 흩어지게 되어 CPU L1/L2 캐시 미스율이 급증한다.
3. **할당 탐색 비용 증가**: 빈 공간을 관리하는 OS 힙의 Free-List가 길어지면서 적당한 크기의 구멍을 찾는(`First-fit` / `Best-fit`) 시간 자체가 길어진다.

### 1.2 멀티스레드 힙 락 경합 (Heap Lock Contention)
표준 `malloc` / `free`는 프로세스 전체가 공유하는 전역 힙(Global Heap) 자료구조를 수정해야 하므로, **내부적으로 동기화 락(Heap Lock)**을 사용한다.
- 만약 16개의 워커 스레드가 초당 수십만 개의 패킷과 `Job` 객체를 동시에 `new` / `delete` 하면, 인게임 로직에는 락이 하나도 없더라도 **메모리 할당기 내부의 힙 락 앞에서 16개 스레드가 전부 줄을 서는 병목**이 발생한다.

---

## 2. Object Pool (오브젝트 풀)과 Placement New

가장 먼저 도입하는 기법은 특정 클래스(예: `Player`, `Monster`, `OverlappedEx`) 전용으로 고정 크기 메모리 블록을 미리 뭉텅이로 만들어 두고 재사용하는 **Object Pool**이다.

### 2.1 핵심 원리: 메모리 할당(`malloc`)과 생성자 호출(`Constructor`)의 분리

C++에서 일반 `new Foo(1, 2)`는 두 가지 일을 한꺼번에 수행한다:
1. `operator new(sizeof(Foo))`를 호출해 힙에서 메모리를 할당받는다.
2. 그 메모리 주소 위에 `Foo::Foo(1, 2)` 생성자를 호출한다.

Object Pool은 **1번(메모리 할당/해제)은 풀(Free-List)에서 $O(1)$로 꺼내고 반납**하되, 객체의 상태 초기화를 위해 **2번(생성자/소멸자 호출)만 C++의 `Placement New` 문법으로 직접 수행**한다.

```cpp
#include <new>
#include <utility>

template<typename Type>
class ObjectPool
{
public:
    template<typename... Args>
    static Type* Pop(Args&&... args)
    {
        // 1. 풀(Lock-Free Stack 또는 TLS Pool)에서 sizeof(Type) 크기의 날것(Raw) 메모리를 꺼냄
        // (풀에 남는 게 없으면 그때만 ::operator new로 새로 할당)
        Type* memory = static_cast<Type*>(s_pool.Pop());

        // 2. Placement New: 이미 할당된 memory 주소 위에 생성자만 호출!
        new(memory) Type(std::forward<Args>(args)...);
        return memory;
    }

    static void Push(Type* obj)
    {
        if (obj == nullptr) return;

        // 1. 소멸자만 명시적으로 직접 호출 (메모리는 OS에 반납하지 않음!)
        obj->~Type();

        // 2. 다 쓴 날것(Raw) 메모리 블록을 다시 풀에 반납하여 다음 Pop 때 재사용
        s_pool.Push(obj);
    }

    // std::shared_ptr와 완벽하게 결합하는 헬퍼 함수!
    // 참조 카운트가 0이 될 때 기본 delete 대신 ObjectPool::Push가 자동 호출되도록 Custom Deleter 지정
    template<typename... Args>
    static std::shared_ptr<Type> MakeShared(Args&&... args)
    {
        return std::shared_ptr<Type>{ Pop(std::forward<Args>(args)...), Push };
    }

private:
    static MemoryPool s_pool; // sizeof(Type) 전용 고정 크기 풀
};
```

> **장점**: 클래스별로 전용 풀을 쓰므로 외부 단편화가 **0%**이며, 만약 메모리 오염 버그가 발생해도 어느 클래스 풀에서 터졌는지 즉시 범인을 특정할 수 있다.

---

## 3. Slab / Size-Class 기반 범용 Memory Pool

`std::vector`, `std::map` 같은 STL 컨테이너나 가변 크기 버퍼는 특정 클래스 타입으로 묶을 수 없으므로, **요청된 바이트 크기(Size Class)별로 미리 준비된 메모리 풀에서 떼어주는 범용 Memory Pool(Slab Allocator)**을 구축한다.

### 3.1 Size-Class 테이블 설계

모든 바이트 크기(1B, 2B, 3B... 4096B)마다 풀을 만들면 풀이 수천 개가 되어 관리가 불가능하다. 따라서 **일정한 간격(예: 32B 단위 $\rightarrow$ 128B 단위 $\rightarrow$ 256B 단위)**으로 구간을 나누어 관리한다.

```text
[Size-Class 기반 Memory Pool 매핑 구조]

요청 크기:  1 ~ 1024 Byte     --->  32 Byte 단위로 끊음 (32B, 64B, 96B ... 1024B 풀: 총 32개)
요청 크기: 1025 ~ 2048 Byte   ---> 128 Byte 단위로 끊음 (1152B, 1280B ... 2048B 풀: 총 8개)
요청 크기: 2049 ~ 4096 Byte   ---> 256 Byte 단위로 끊음 (2304B, 2560B ... 4096B 풀: 총 8개)
요청 크기: 4097 Byte 초과     ---> 너무 큰 메모리는 풀링하지 않고 OS 기본 malloc/free 직접 호출!
```

### 3.2 `MemoryHeader`를 통한 크기 추적

`free(void* ptr)`처럼 포인터 주소만 넘겨받아도 이 메모리가 몇 바이트짜리 풀에서 나왔는지 즉시 알 수 있도록, 실제 유저에게 돌려주는 주소 바로 앞단에 작은 **`MemoryHeader`**를 붙여서 할당한다.

```text
[메모리 블록 내부 레이아웃 (예: 유저가 20 Byte를 요청했을 때 -> 32 Byte 풀에서 할당)]

+----------------------------+-----------------------------------------+
|   MemoryHeader (8~16 Byte) |        실제 유저 데이터 영역 (20B + 여분)  |
|   [ allocSize = 36 Byte ]  |                                         |
+----------------------------+-----------------------------------------+
^                            ^
|                            |
실제 풀에서 꺼낸 시작 주소     유저에게 반환하는 포인터 (ptr)
(반납 시 ptr - sizeof(MemoryHeader) 위치로 이동하여 allocSize를 읽고 원래 풀에 반납!)
```

```cpp
struct MemoryHeader
{
    int32_t allocSize; // 헤더를 포함한 전체 할당 크기

    // 헤더 뒤에 이어지는 실제 유저 데이터 영역의 시작 주소를 반환
    static void* AttachHeader(MemoryHeader* header, int32_t size)
    {
        header->allocSize = size;
        return reinterpret_cast<void*>(++header); // sizeof(MemoryHeader)만큼 전진한 주소
    }

    // 유저 포인터로부터 앞단의 MemoryHeader 주소를 역산하여 추출
    static MemoryHeader* DetachHeader(void* ptr)
    {
        MemoryHeader* header = reinterpret_cast<MemoryHeader*>(ptr);
        return --header; // sizeof(MemoryHeader)만큼 후진한 주소
    }
};
```

---

## 4. TLS (Thread Local Storage) 메모리 풀 (TCMalloc / jemalloc의 원리)

Size-Class 기반 Memory Pool을 만들고 각 풀을 Lock-Free Stack(`SLIST_HEADER` 등)으로 구현하더라도, 16개의 스레드가 매번 전역 Memory Pool의 `top` 포인터 하나에 대해 CAS(원자적 연산)를 수행하면 **캐시 라인 핑퐁(Cache Line Ping-Pong)**으로 인한 경합 비용이 여전히 남는다.

Google의 **TCMalloc(Thread-Caching Malloc)**이나 Meta가 애용하는 **jemalloc**이 압도적인 속도를 내는 비결은 바로 **TLS(Thread Local Storage)에 각 스레드만의 전용 로컬 캐시 풀을 두어 전역 락과 CAS조차 아예 없애버리는 계층적(2-Tier) 구조**다.

```text
[TLS 캐시 + Central Pool 2단계 할당 아키텍처]

  Thread 1                      Thread 2                      Thread 3
+-------------------+         +-------------------+         +-------------------+
| TLS Local Cache   |         | TLS Local Cache   |         | TLS Local Cache   |
| (락/CAS 없이 즉시 |         | (락/CAS 없이 즉시 |         | (락/CAS 없이 즉시 |
|  할당/반납: O(1)) |         |  할당/반납: O(1)) |         |  할당/반납: O(1)) |
+-------------------+         +-------------------+         +-------------------+
          ^                             ^                             ^
          | (바닥났을 때만 100개씩      | (너무 많이 쌓였을 때만      |
          |  뭉텅이로 가져옴: Batch)    |  100개씩 뭉텅이로 반납)     |
          v                             v                             v
+-------------------------------------------------------------------------------+
|                     Central Memory Pool (전역 중앙 저장소)                     |
|                     (뭉텅이 이동 시에만 가끔 동기화 발생)                     |
+-------------------------------------------------------------------------------+
```

### 동작 메커니즘
1. **평상시 할당 (`Alloc`)**:
   - 각 워커 스레드는 자신의 `thread_local` 변수로 들고 있는 **TLS Free-List**에서 메모리를 꺼낸다.
   - 오직 자기 자신만 접근하는 메모리이므로 **Mutex도, Atomic CAS도 전혀 필요 없는 단순 포인터 대입(`ptr = head; head = head->next;`)만으로 약 2~3 나노초 만에 할당이 끝난다!**
2. **TLS 캐시가 바닥났을 때 (Batch Refill)**:
   - 내 TLS Free-List가 비었을 때만 중앙의 `Central Memory Pool`에 접근하여 1개가 아니라 **한 번에 100개(Chunk 묶음)를 통째로 가져와** 내 TLS에 채워 넣는다. (전역 경합 횟수가 $1/100$로 줄어든다.)
3. **반납 시 (`Release`)**:
   - 스레드 A에서 할당한 패킷을 스레드 B가 다 쓰고 해제하더라도, 스레드 B는 자신의 **TLS Free-List**에 그냥 넣는다.
   - 만약 스레드 B의 TLS에 빈 블록이 일정 임계치(예: 200개) 이상 과도하게 쌓이면, 그때 100개를 묶어서 다시 `Central Memory Pool`로 돌려보내어 스레드 간 메모리 불균형을 해소한다.

---

## 5. 메모리 안정성 검증: Stomp Allocator (스톰프 할당자)

Object Pool이나 Memory Pool을 도입하면 성능은 비약적으로 상승하지만, 치명적인 부작용이 하나 생긴다.
> **"메모리를 OS에 진짜로 반납(`free`)하지 않고 풀 안에서 계속 재사용하기 때문에, 이미 반납된 객체를 건드리는 Use-After-Free(댕글링 포인터) 버그나 배열 범위를 살짝 넘어서는 Buffer Overflow 버그가 발생해도 당장 크래시가 안 나고 몇 시간 뒤에 엉뚱한 곳에서 데이터가 오염된 채로 터진다!"**

이 악명 높은 메모리 오염 버그를 **발생 즉시(해당 명령어 줄에서 정확히!) 검거**하기 위해 개발/디버그 모드에서 메모리 풀 대신 켜서 사용하는 도구가 **Stomp Allocator(Pageheap 기법)**다.

---

### 5.1 Stomp Allocator의 하드웨어적 원리

CPU의 MMU(Memory Management Unit)가 가상 메모리의 읽기/쓰기 권한을 감시하는 최소 단위는 **페이지(Page, 보통 4KB = 4096 Byte)**다. Stomp Allocator는 이 하드웨어 페이지 보호 기능을 역이용한다.

#### ① Use-After-Free (해제 후 접근) 즉시 검거
- 객체를 할당할 때마다 메모리 풀이 아니라 OS의 가상 메모리 시스템 콜(`VirtualAlloc` / `mmap`)을 직접 호출해 **4KB 페이지 전체**를 할당한다.
- 객체가 해제(`Release`)되는 순간, `VirtualFree(..., MEM_RELEASE)` (또는 `mprotect(..., PROT_NONE)`)를 호출해 **해당 페이지의 가상 메모리 매핑을 즉시 박탈**해 버린다.
- 만약 어디선가 남아있던 댕글링 포인터가 해제된 객체에 단 1바이트라도 접근을 시도하면, CPU 하드웨어가 즉시 **Page Fault (Access Violation `0xC0000005` / `SIGSEGV`)**를 발생시켜 **범인이 되는 정확한 코드 라인에서 디버거를 멈춰 세운다!**

#### ② Buffer Overflow (메모리 끝단 침범) 즉시 검거
유저가 20바이트짜리 객체를 요청했는데 4KB 페이지의 맨 앞(0번 오프셋)에 붙여 주면, 실수로 24바이트를 써서 4바이트 오버플로우가 나더라도 여전히 4KB 페이지 내부이므로 하드웨어 예외가 발생하지 않는다.
- **해결책**: 요청한 메모리를 **4KB 페이지의 맨 끝단(오른쪽 벽)에 딱 붙여서 배치**하고, 바로 다음 페이지는 접근 불가(Guard Page / Unmapped) 상태로 비워둔다!

```text
[Stomp Allocator의 Buffer Overflow 검거를 위한 메모리 배치]

|<------------------- 할당된 4KB 페이지 (4096 Byte) ------------------->|<-- 다음 페이지 (Unmapped) -->|
+--------------------------------------------------+--------------------+------------------------------+
|          앞부분 남는 공간 (4076 Byte)             | 요청한 객체 (20B)  |   접근 시 즉시 CRASH!        |
+--------------------------------------------------+--------------------+------------------------------+
^                                                  ^                    ^
baseAddress (4KB 정렬)                              유저에게 반환하는 주소  페이지 경계 (1바이트만 넘어도
                                                   (4096 - 20 = 4076)    하드웨어 Access Violation!)
```

### 5.2 Windows 기준 Stomp Allocator 핵심 구현 코드

```cpp
class StompAllocator
{
    static constexpr uint64_t PAGE_SIZE = 0x1000; // 4096 Bytes (4KB)

public:
    static void* Alloc(int32_t size)
    {
        // 1. 필요한 페이지 개수 계산: (size + 4095) / 4096
        const uint64_t pageCount = (static_cast<uint64_t>(size) + PAGE_SIZE - 1) / PAGE_SIZE;

        // 2. 페이지 끝단에 딱 붙이기 위한 시작 오프셋 계산 (Overflow 즉시 감지용!)
        const uint64_t dataOffset = pageCount * PAGE_SIZE - size;

        // 3. OS로부터 가상 메모리 페이지 직접 예약 및 커밋
        void* baseAddress = ::VirtualAlloc(
            nullptr, 
            pageCount * PAGE_SIZE, 
            MEM_RESERVE | MEM_COMMIT, 
            PAGE_READWRITE
        );

        // 페이지 끝단에 맞춘 주소를 반환
        return static_cast<void*>(static_cast<uint8_t*>(baseAddress) + dataOffset);
    }

    static void Release(void* ptr)
    {
        // 1. 반환받은 포인터 주소에서 하위 12비트(4096으로 나눈 나머지)를 버리면 원래 페이지 시작 주소가 나옴!
        const uint64_t address = reinterpret_cast<uint64_t>(ptr);
        const uint64_t baseAddress = address - (address % PAGE_SIZE);

        // 2. 페이지 매핑을 즉시 해제 -> 이후 ptr에 접근하면 100% Access Violation 크래시 발생!
        ::VirtualFree(reinterpret_cast<void*>(baseAddress), 0, MEM_RELEASE);
    }
};
```
> **실무 활용**: 전처리기 매크로(`#ifdef _DEBUG`)나 서버 설정 파일을 통해, 평소 개발 및 Chaos 부하 테스트(Step 2 과제)를 돌릴 때는 `StompAllocator`를 켜서 댕글링 포인터와 오버플로우를 싹 다 잡아내고, 라이브 릴리즈 빌드에서는 `TLS MemoryPool`로 스위칭하여 최고 성능을 낸다.

---

## 요약 체크리스트 (면접 및 실습 핵심 포인트)

1. **커스텀 할당자의 목적**: 빈번한 `new`/`delete`로 인한 **힙 외부 단편화(Fragmentation)**와 멀티스레드 환경의 **전역 힙 락(Heap Lock) 경합**을 제거하고 캐시 지역성을 높이기 위해 사용한다.
2. **Object Pool과 Placement New**: 메모리는 풀에서 재사용하되, 객체의 생성자와 소멸자는 **`new(ptr) Type(...)` (Placement New)**와 `ptr->~Type()`으로 직접 호출하며, `std::shared_ptr`의 Custom Deleter와 결합해 안전하게 자동 반납되도록 설계한다.
3. **TLS 메모리 풀**: 전역 풀의 CAS 경합마저 없애기 위해 각 스레드의 **TLS(Thread Local Storage)**에 로컬 캐시를 두고, 바닥나거나 넘칠 때만 중앙 풀과 뭉텅이(Batch)로 주고받는 방식을 사용한다.
4. **Stomp Allocator**: 풀링 사용 시 은폐되기 쉬운 **Use-After-Free**와 **Buffer Overflow** 버그를 잡기 위해, 디버그 시에는 `VirtualAlloc`으로 4KB 페이지의 **맨 끝단에 메모리를 배치**하고 해제 시 페이지를 즉시 날려버려 하드웨어 예외를 유도한다.
