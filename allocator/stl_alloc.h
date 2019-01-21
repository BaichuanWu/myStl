#ifndef STL_ALLOC_H
#define STL_ALLOC_H

#if 0
#	include<new>
#	define __THROW_BAD_ALLOC throw bad_alloc
#elif !defined(__THROW_BAD_ALLOC)
#	include<iostream>
#	define __THROW_BAD_ALLOC std::cerr<<"out of memory"<<std::endl;
#endif
#include<cstdlib>
#include<new>

template<int inst>
class __malloc_alloc_template{
	private:
		static void *oom_malloc(size_t);
		static void *oom_realloc(void *, size_t);
		static void (* __malloc_alloc_oom_handler) (); // 实现c++ new handler
	public:
		static void *allocate(size_t n) {
			void *result = malloc(n);
			if (result==0) result=oom_malloc(n);
			return result;
		}

		static void deallocate(void *p, size_t){
			free(p);
		}

		static void *reallocate(void *p, size_t, size_t new_sz){
			void *result = realloc(p, new_sz);
			if (result==0) result=oom_realloc(p, new_sz);
			return result;
		}

		static void (*set_malloc_handler(void (*f)())) (){ //为啥不typedef 下?
			void (*old) () = __malloc_alloc_oom_handler;
			__malloc_alloc_oom_handler = f;
			return (old);
		}
};

template <int inst>
void (* __malloc_alloc_template<inst>::__malloc_alloc_oom_handler) ()=0; 

template<int inst>
void *__malloc_alloc_template<inst>::oom_malloc(size_t n) {
	void (*my_malloc_handler)();
	void *result;
	for (;;){
		my_malloc_handler = __malloc_alloc_oom_handler;
		if (my_malloc_handler==0) {__THROW_BAD_ALLOC;}
		my_malloc_handler();
		result = malloc(n);
		if (result) return result;
	}
}
template<int inst>
void *__malloc_alloc_template<inst>::oom_realloc(void *p, size_t n) {
	void (*my_malloc_handler)();
	void *result;
	for (;;){
		my_malloc_handler = __malloc_alloc_oom_handler;
		if (my_malloc_handler==0) {__THROW_BAD_ALLOC;}
		my_malloc_handler();
		result = realloc(p, n);
		if (result) return result;
	}
}
typedef __malloc_alloc_template<0> malloc_alloc;

enum {__ALIGN=8};
enum {__MAX_BYTE=128};
enum {__NFREELISTS=__MAX_BYTE / __ALIGN};

template <bool threads, int inst>
class __default_alloc_template{
	private:
		static size_t ROUND_UP(size_t bytes){
			return (bytes + __ALIGN -1) & (__ALIGN-1);
		}

		union obj {
			union obj * free_list_link;
			char client_data[1];
		};

		static obj * volatile free_list[__NFREELISTS];
		static size_t FREELIST_INDEX(size_t bytes){
			return (bytes + __ALIGN-1)/(__ALIGN-1);
		}
		static void *refill(size_t);    //无可用free_list时调用，重新填充free_list，配置一块理论上nobjs个大小为传参的空间
		static char *chunk_alloc(size_t size,int &nobjs);  //从内存池获取内存的具体方法
		// chunk_alloc相关的内存池属性
		static char *start_free;
		static char *end_free;
		static size_t heap_size;
	public:
		static void *allocate(size_t n){
			/*大致逻辑就是大于128走一级分配
			 * 如果free_list中存在则取，并且更新链表
			 * 没有的话通过refill重新配置一大块区域
			 * */
			obj * volatile *my_free_list;
			obj *result;
			if (n>size_t(__MAX_BYTE)) {
				return malloc_alloc::allocate(n);
			}
			my_free_list = free_list + FREELIST_INDEX(n);
			result = *my_free_list;
			if (result==0){
				void *r=refill(ROUND_UP(n));
				return r;
			}
			*my_free_list = result->free_list_link;
			return result;
		}
		static void deallocate(void *p, size_t n) {
			obj *q=(obj *)p;
			obj *volatile *my_free_list;
			if (n>(size_t) __MAX_BYTE){
				malloc_alloc::deallocate(p, n);
				return;
			}
			my_free_list = free_list + FREELIST_INDEX(n);
			q->free_list_link = *my_free_list;
			*my_free_list = q;
		}
		static void *reallocate(void *p, size_t old_sz, size_t new_sz);
};
// static的一些初始化
template<bool threads, int inst>
char *__default_alloc_template<threads, inst>::start_free=0;
template<bool threads, int inst>
char *__default_alloc_template<threads, inst>::end_free=0;
template<bool threads, int inst>
size_t __default_alloc_template<threads, inst>::heap_size=0;

template<bool threads, int inst> 
typename __default_alloc_template<threads, inst>::obj *volatile 
__default_alloc_template<threads, inst>::free_list[__NFREELISTS]=
{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};

template<bool threads, int inst>
void * __default_alloc_template<threads, inst>::refill(size_t n){
	int nobjs=20;
	obj * volatile *my_free_list;
	obj *result;
	obj *current_obj, *next_obj;
	int i;
	char *chunk = chunk_alloc(n, nobjs); //传的是int引用会更新
	if (1==nobjs) return (chunk); //只有一个的话不需要调整free_list
	my_free_list = free_list + FREELIST_INDEX(n);
	result = (obj *) chunk;
	*my_free_list = next_obj=(obj*) (chunk +n);
	for (i=1;;i++){
		current_obj = next_obj;
		next_obj = (obj*) (chunk +n);
		if (nobjs-1==i){
			current_obj->free_list_link=0;
			break;
		} else {
			current_obj->free_list_link=next_obj;
		}
	}
	return result;
}

template<bool threads, int inst>
char *
__default_alloc_template<threads, inst>::chunk_alloc(size_t size, int& nobjs){
	//一些分之递归调用自己是为了修正nobjs
	//涉及多线程的被忽略了
	char *result;
	size_t total_bytes=size*nobjs;
	size_t bytes_left = end_free-start_free;

	if (bytes_left>=total_bytes){
		result = start_free;
		start_free += total_bytes;
		return result;
	} else if (bytes_left >= size) {
		// 不能满足，但至少能提供一个
		nobjs = bytes_left/size;
		total_bytes = nobjs * size;
		result = start_free;
		start_free+=total_bytes;
		return result;
	} else {
		size_t bytes_to_get = 2 * total_bytes + ROUND_UP(heap_size>>4);
		if (bytes_left>0){
			//如果还有可用内存，先分配给free_list上小于其的最大链表维护的空间(可以优化)
			obj * volatile *my_free_list = free_list + FREELIST_INDEX(bytes_left);
			((obj *)start_free)->free_list_link = *my_free_list;
			*my_free_list = (obj *)start_free;
		}
		start_free = (char *) malloc(bytes_to_get);
		if (0==start_free){
			//如果heap无可用内存，尝试从更大的free_list上获取内存
			int i;
			obj *volatile *my_free_list, *p;
			for (i=size;i<=__MAX_BYTE;i+=__ALIGN){
				my_free_list = free_list + FREELIST_INDEX(i);
				p = *my_free_list;
				if (0!=p){
					*my_free_list=p->free_list_link;
					start_free = (char *) p;
					end_free = start_free+i;
					return chunk_alloc(size, nobjs);
				}
			}
			end_free = 0;
			start_free = (char *) malloc_alloc::allocate(bytes_to_get); //理论上会导致触发set_malloc_handler
		}
		heap_size += bytes_to_get;
		end_free = start_free + bytes_to_get;
		return chunk_alloc(size, nobjs);
	}
}

#endif
