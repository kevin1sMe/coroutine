
#include "coroutine.h"
#include <stdio.h>
#include <stdlib.h>

#ifndef _ON_MAC_
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif

#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>

#define STACK_SIZE (1024*1024)
#define DEFAULT_COROUTINE 16

struct coroutine;

struct schedule {
	char stack[STACK_SIZE]; //1M的栈空间
	ucontext_t main; //main的上下文
	int nco; //已经使用的co
	int cap; //总共co 的数量,　即将会malloc(cap*sizeof(coroutine*)))大小内存
	int running; // schedule状态, 正在执行哪个coroutine.. = id
	struct coroutine **co; //保存所有的coroutine信息
};

struct coroutine {
	coroutine_func func; //协程所调用函数  
                    //typedef void (*coroutine_func)(struct schedule *, void *ud);
	void *ud;   //user data
	ucontext_t ctx; //此协程上下文
	struct schedule * sch; //此coroutine从属的schedule
	ptrdiff_t cap;
	ptrdiff_t size;
	int status; //此coroutine的状态，是正在运行，还是挂起
                //#define COROUTINE_DEAD 0
                //#define COROUTINE_READY 1
                //#define COROUTINE_RUNNING 2
                //#define COROUTINE_SUSPEND 3


	char *stack;
};

//创建和初始化一个coroutine,　内部使用
struct coroutine * 
_co_new(struct schedule *S , coroutine_func func, void *ud) {
    printf("%s:%s\n", __FILE__, __FUNCTION__);
	struct coroutine * co = malloc(sizeof(*co));
	co->func = func;
	co->ud = ud;
	co->sch = S;
	co->cap = 0;
	co->size = 0;
	co->status = COROUTINE_READY; //创建完进入READY状态，等resume()
	co->stack = NULL;
	return co;
}

//删除一个coroutine
void
_co_delete(struct coroutine *co) {
    printf("%s:%s\n", __FILE__, __FUNCTION__);
	free(co->stack); //因创建时已经置空，故可直接free
    // co->ud 不用删除？　算是由外界传入，非吾所管理
	free(co);
}

//创建一个schedule, 感觉改名叫schedule_new或许更合适？
struct schedule * 
coroutine_open(void) {
    printf("%s:%s\n", __FILE__, __FUNCTION__);
	struct schedule *S = malloc(sizeof(*S));
	S->nco = 0;
	S->cap = DEFAULT_COROUTINE;
	S->running = -1; //-1是未开始吧
	S->co = malloc(sizeof(struct coroutine *) * S->cap); //分配空间存指针
	memset(S->co, 0, sizeof(struct coroutine *) * S->cap);
	return S;
}

//关闭schedule，释放所有的coroutine
void 
coroutine_close(struct schedule *S) {
    printf("%s:%s\n", __FILE__, __FUNCTION__);
	int i;
	for (i=0;i<S->cap;i++) {
		struct coroutine * co = S->co[i];
		if (co) {
			_co_delete(co);
		}
	}
	free(S->co);
	S->co = NULL;
	free(S);
}

//创建一个coroutine,并返回其id
int 
coroutine_new(struct schedule *S, coroutine_func func, void *ud) {
    printf("%s:%s\n", __FILE__, __FUNCTION__);
    //调用内部函数创建coroutine
	struct coroutine *co = _co_new(S, func , ud);

    //将创建的coroutine加入到schedule中管理
	if (S->nco >= S->cap) {
		int id = S->cap;
		S->co = realloc(S->co, S->cap * 2 * sizeof(struct coroutine *));
		memset(S->co + S->cap , 0 , sizeof(struct coroutine *) * S->cap);
		S->co[S->cap] = co;
		S->cap *= 2;
		++S->nco;
		return id;
	} else {
		int i;
        //这里从前向后查找空闲的位置并使用,看来此id会很快被复用的
		for (i=0;i<S->cap;i++) {
			int id = (i+S->nco) % S->cap;
			if (S->co[id] == NULL) {
				S->co[id] = co;
				++S->nco;
				return id;
			}
		}
	}
	assert(0);
	return -1;
}

//从这里，调用coroutine的callback,当callback()返回后，此coroutine即结束
static void
mainfunc(uint32_t low32, uint32_t hi32) {
    printf("%s:%s\n", __FILE__, __FUNCTION__);
    //为啥整了个这样的东东，　ptr -- schedule*
	uintptr_t ptr = (uintptr_t)low32 | ((uintptr_t)hi32 << 32);
	struct schedule *S = (struct schedule *)ptr;
	int id = S->running;//可能-1吗, 目前是调用此函数前就设置了此次croutine id
	struct coroutine *C = S->co[id];
	C->func(S,C->ud); //调用coroutine的callback(?)

    //当func()调用结束后，此co即可宣告死亡
	_co_delete(C);
	S->co[id] = NULL;
	--S->nco;
	S->running = -1; //-1代表当前没有coroutine在执行？
}

//运行coroutine, resume是叫唤醒的意思吗，重新回到。。
void 
coroutine_resume(struct schedule * S, int id) {
    printf("%s:%s\n", __FILE__, __FUNCTION__);
    printf("coroutine:[%d]\n", id);
	assert(S->running == -1);
	assert(id >=0 && id < S->cap);
	struct coroutine *C = S->co[id];
	if (C == NULL)
		return;
	int status = C->status;
	switch(status) {
	case COROUTINE_READY:
        printf("coroutine:[%d] COROUTINE_READY\n", id);
		getcontext(&C->ctx);//这个调用 ，将会使ctx内的东西和当前的上下文关联上（ss,sp??)
		C->ctx.uc_stack.ss_sp = S->stack; //要点： 对于context,  stack需要自己分配给它,这里用S->stack传过去，相当于设置了堆栈的指针，如此使函数的调用过程中的栈信息放到了我们想要保存的位置 
		C->ctx.uc_stack.ss_size = STACK_SIZE;
		C->ctx.uc_link = &S->main; //这个是当这个context返回时, resume的目标
		S->running = id;//正在运行croutine id, 如此看来这块是非线程安全?
		C->status = COROUTINE_RUNNING;
		uintptr_t ptr = (uintptr_t)S;
        //这里为何要将uintptr分为两个uint32_t传递？ --> man makecontext
		makecontext(&C->ctx, (void (*)(void)) mainfunc, 2, (uint32_t)ptr, (uint32_t)(ptr>>32));
        //将当前的上下文保存到S->main中，并且控制权交给C->ctx. 即去执行ctx中设置的函数等等
		swapcontext(&S->main, &C->ctx);
        printf("coroutine:[%d] COROUTINE_READY swapcontext return\n", id);
		break;
	case COROUTINE_SUSPEND:
        printf("coroutine:[%d] COROUTINE_SUSPEND\n", id);
        //将要唤醒的croutine的stack拷贝到S->stack末尾去，这是何意？为什么是末尾
		memcpy(S->stack + STACK_SIZE - C->size, C->stack, C->size);
		S->running = id;
		C->status = COROUTINE_RUNNING;
		swapcontext(&S->main, &C->ctx);
        printf("coroutine:[%d] COROUTINE_SUSPEND swapcontext\n", id);
		break;
	default:
		assert(0);
	}
}

//保存croutine的stack内容
static void
_save_stack(struct coroutine *C, char *top) {
    printf("%s:%s top:%p\n", __FILE__, __FUNCTION__, top);
    //在栈上创建一个局部变量, 以这个变量的位置为基准，往下的那一段都是函数调用的堆栈
	char dummy = 0;
	assert(top - &dummy <= STACK_SIZE);
	if (C->cap < top - &dummy) {
		free(C->stack);
		C->cap = top-&dummy;
		C->stack = malloc(C->cap); //C->stack用来保存coroutine自身的栈内容
	}
	C->size = top - &dummy; //计算出需要保存的栈长度
    printf("%s:%s C->stack:%p dummy:%p C->size:%ld\n", 
            __FILE__, __FUNCTION__, C->stack, &dummy, C->size);
	memcpy(C->stack, &dummy, C->size);
}


void
coroutine_yield(struct schedule * S) {
    printf("%s:%s\n", __FILE__, __FUNCTION__);
	int id = S->running;
	assert(id >= 0);
    //取出正在运行的croutine
	struct coroutine * C = S->co[id];
    //这个assert()是何意？ 这里的S->stack为何是栈上的地址？不是从堆上分配的吗
    printf("&C:%p S->stack:%p\n", &C, S->stack);
	assert((char *)&C > S->stack);
    //保存这个coroutine的stack
	_save_stack(C,S->stack + STACK_SIZE);
    //使进入SUSPEND状态
	C->status = COROUTINE_SUSPEND; 
	S->running = -1;
    //切换回S->main, 就是当初从coroutine_resume()中过来的那里
	swapcontext(&C->ctx , &S->main);
}

//获取某个coroutine的状态
int 
coroutine_status(struct schedule * S, int id) {
    printf("%s:%s\n", __FILE__, __FUNCTION__);
	assert(id>=0 && id < S->cap);
	if (S->co[id] == NULL) {
		return COROUTINE_DEAD;
	}
	return S->co[id]->status;
}

//获得正在运行的croutine id
int 
coroutine_running(struct schedule * S) {
    printf("%s:%s\n", __FILE__, __FUNCTION__);
	return S->running;
}

