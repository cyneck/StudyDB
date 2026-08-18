CC = gcc
CFLAGS = -std=c99 -g -Wall -Isrc
LIBS += -lm
BUILD = ./build
SRC = src

# programs to build (names only, without path or .c)
PROGRAM = test_expr test_assign3_1 test_btree test_ddl test_dml demo_api demo_sql
EXCEPT =

# all .c files under src/ → strip path and .c → plain names
_ALL_C = $(wildcard $(SRC)/*.c)
_ALL_NAMES = $(patsubst $(SRC)/%.c,%,$(_ALL_C))

# dependency object names (everything except the programs themselves)
_DEPS_NAMES = $(filter-out $(PROGRAM) $(EXCEPT), $(_ALL_NAMES))
DEPS = $(addprefix $(BUILD)/, $(addsuffix .o, $(_DEPS_NAMES)))

.PHONY: all
all: $(PROGRAM)

# compile: build/foo.o <- src/foo.c
$(BUILD)/%.o: $(SRC)/%.c
	if [ ! -d $(BUILD) ]; then mkdir $(BUILD); fi
	$(CC) $(CFLAGS) -c -o $@ $^ $(LIBS)

# link: build/<program> <- build/<program>.o + all DEPS
$(PROGRAM): $(patsubst %, $(BUILD)/%, $(addsuffix .o, $(PROGRAM))) $(DEPS)
	$(CC) $(CFLAGS) -o $(BUILD)/$@ $(BUILD)/$@.o $(DEPS) $(LIBS)

.PHONY: clean
clean:
	rm -f $(BUILD)/*
