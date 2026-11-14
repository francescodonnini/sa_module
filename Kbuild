obj-m += shdwstck_runtime.o shdwstck_debug.o

shdwstck_runtime-objs := main.o \
                         core/alloc.o \
                         core/core.o \
                         core/device.o \
                         core/layout.o \
                         core/pivot.o \
                         core/pt_bindings.o \
                         core/shadow_pt.o \
                         core/stack_vma.o \
						 core/pt_bindings.o

shdwstck_debug-objs := core/debug.o

ccflags-y += -I$(src)/include
CFLAGS_main.o += -DDEBUG