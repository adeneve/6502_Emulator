// virtual 6502 processor
// can do an exercise to create an assembler that runs on the 6502, then a 'smallC' compiler after that

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// 1. CPU STRUCTURE & STATE DEFINITIONS
// ============================================================================

typedef struct {
    uint16_t PC;        // Program Counter
    uint8_t  SP;        // Stack Pointer
    uint8_t  A;         // Accumulator
    uint8_t  X;         // X Index Register
    uint8_t  Y;         // Y Index Register
    uint8_t  Status;    // Status Flags: N V _ B D I Z C
    uint32_t Cycles;    // Total clock cycles elapsed
} CPU_6502;

// 64KB System RAM
uint8_t RAM[65536];

// Flag Masks
#define FLAG_C (1 << 0) // Carry
#define FLAG_Z (1 << 1) // Zero
#define FLAG_I (1 << 2) // Interrupt Disable
#define FLAG_D (1 << 3) // Decimal Mode
#define FLAG_B (1 << 4) // Break Command
#define FLAG_U (1 << 5) // Unused/Always 1 on stack
#define FLAG_V (1 << 6) // Overflow
#define FLAG_N (1 << 7) // Negative

// Memory Read/Write Wrappers (Crucial for hardware-mapped I/O tracking)
uint8_t cpu_read(uint16_t address) {
    return RAM[address];
}

void cpu_write(uint16_t address, uint8_t data) {
    RAM[address] = data;
}

// System Reset Procedure
void reset_cpu(CPU_6502* cpu) {
    // Read starting address from the 6502 Reset Vector (0xFFFC-0xFFFD)
    uint16_t low = cpu_read(0xFFFC);
    uint16_t high = cpu_read(0xFFFD);
    cpu->PC = (high << 8) | low;

    cpu->SP = 0xFD; // Hardware reset default stack offset
    cpu->A = 0;
    cpu->X = 0;
    cpu->Y = 0;
    cpu->Status = FLAG_U | FLAG_I; // Unused flag is always high; interrupts disabled
    cpu->Cycles = 0;
}

// ============================================================================
// 2. STACK OPERATIONS MODULE
// ============================================================================

// The 6502 stack hardwires directly to Memory Page 1 (0x0100 - 0x01FF)
void stack_push(CPU_6502* cpu, uint8_t value) {
    cpu_write(0x0100 + cpu->SP, value);
    cpu->SP--;
}

uint8_t stack_pop(CPU_6502* cpu) {
    cpu->SP++;
    return cpu_read(0x0100 + cpu->SP);
}

// ============================================================================
// 3. ADDRESSING MODES MODULE
// ============================================================================
// Returns the exact 16-bit target address in memory where the data lives.
// If an addressing mode can consume an extra clock cycle due to a page boundary 
// crossing (e.g. crossing from 0x01FF to 0x0200), it returns true.

bool mode_IMM(CPU_6502* cpu, uint16_t* target_address) {
    *target_address = cpu->PC;
    cpu->PC++;
    return false;
}

bool mode_ZP0(CPU_6502* cpu, uint16_t* target_address) {
    *target_address = cpu_read(cpu->PC);
    cpu->PC++;
    return false; // Zero-page addresses never cross 256-byte boundaries
}

bool mode_ZPX(CPU_6502* cpu, uint16_t* target_address) {
    uint8_t base = cpu_read(cpu->PC);
    *target_address = (uint8_t)(base + cpu->X); // Wraps around zero page automatically
    cpu->PC++;
    return false;
}

bool mode_ABS(CPU_6502* cpu, uint16_t* target_address) {
    uint16_t low = cpu_read(cpu->PC);
    uint16_t high = cpu_read(cpu->PC + 1);
    *target_address = (high << 8) | low;
    cpu->PC += 2;
    return false;
}

bool mode_ABX(CPU_6502* cpu, uint16_t* target_address) {
    uint16_t low = cpu_read(cpu->PC);
    uint16_t high = cpu_read(cpu->PC + 1);
    uint16_t base = (high << 8) | low;
    *target_address = base + cpu->X;
    cpu->PC += 2;
    
    // Check if adding X caused a page cross (high byte changed)
    return (*target_address & 0xFF00) != (base & 0xFF00);
}

bool mode_IND_Y(CPU_6502* cpu, uint16_t* target_address) {
    uint8_t ptr = cpu_read(cpu->PC); //zero page pointer
    uint16_t low = cpu_read(ptr); 
    uint16_t high = cpu_read(ptr + 1);
    uint16_t base = (high << 8) | low;

    *target_address = base + cpu->Y;
    cpu->PC += 1;
    
    // Check if adding Y caused a page cross (high byte changed)
    return (*target_address & 0xFF00) != (base & 0xFF00);
}

bool mode_REL(CPU_6502* cpu, uint16_t* target_address) {

}

// ============================================================================
// 4. INSTRUCTION SET MODULE (Opcodes Execution & Flag Math)
// ============================================================================

void set_zn(CPU_6502* cpu, uint8_t val) {
    if (val == 0)    cpu->Status |= FLAG_Z; else cpu->Status &= ~FLAG_Z;
    if (val & 0x80)  cpu->Status |= FLAG_N; else cpu->Status &= ~FLAG_N;
}

// ADC: Add with Carry (Handles complex signed/unsigned overflow flags)
// always clear carry before addition CLC
void ins_ADC(CPU_6502* cpu, uint16_t addr) {
    uint8_t data = cpu_read(addr);
    uint8_t carry = (cpu->Status & FLAG_C) ? 1 : 0;
    uint16_t sum = cpu->A + data + carry;

    // Set Carry flag if result exceeds 8 bits (255)
    if (sum > 0xFF) cpu->Status |= FLAG_C; else cpu->Status &= ~FLAG_C;

    // Signed Overflow Formula: (A ^ Sum) & (Data ^ Sum) has bit 7 set
    if (~(cpu->A ^ data) & (cpu->A ^ sum) & 0x80) {
        cpu->Status |= FLAG_V;
    } else {
        cpu->Status &= ~FLAG_V;
    }

    cpu->A = (uint8_t)(sum & 0xFF);
    set_zn(cpu, cpu->A);
}

// CMP: Compare (Compares value in address to accumulator, sets Z flag if equal)
void ins_CMP(CPU_6502* cpu, uint16_t addr) {

    cpu->Status &= ~FLAG_Z;
    cpu->Status &= ~FLAG_C;
    cpu->Status &= ~FLAG_N;

    uint8_t data = cpu_read(addr);
    uint8_t result = cpu->A - data;
    if(result == 0)
    {
        cpu->Status |= FLAG_Z;
    }
    else if(result > 0)
    {
        cpu->Status |= FLAG_C;
    }else{
        cpu->Status |= FLAG_N;
    }
}

// BRE: Branch Equal
void ins_BEQ(CPU_6502* cpu, uint16_t addr) {
    if((cpu->Status & FLAG_Z) == FLAG_Z)
    {
        uint8_t data = cpu_read(cpu->PC);
        cpu->PC += 1;
        cpu->PC += data;
    }
    else{
        cpu->PC += 1;
    }
}

// JSR: Jump to Subroutine (Pushes PC to stack, jumps to target address)
void ins_JSR(CPU_6502* cpu, uint16_t addr) {
    uint16_t return_addr = cpu->PC - 1; // 6502 standard pushes return address minus 1
    stack_push(cpu, (return_addr >> 8) & 0xFF); // High Byte
    stack_push(cpu, return_addr & 0xFF);        // Low Byte
    cpu->PC = addr;
}

// JMP: Jump
void ins_JMP(CPU_6502* cpu, uint16_t addr) {
    cpu->PC = addr;
}

// RTS: Return from Subroutine (Pulls PC from stack)
void ins_RTS(CPU_6502* cpu, uint16_t addr) {
    uint16_t low = stack_pop(cpu);
    uint16_t high = stack_pop(cpu);
    cpu->PC = ((high << 8) | low) + 1;
}

// BRK: Break Instruction (Software Interrupt)
void ins_BRK(CPU_6502* cpu, uint16_t addr) {
    cpu->PC++; // Skip padding byte
    stack_push(cpu, (cpu->PC >> 8) & 0xFF);
    stack_push(cpu, cpu->PC & 0xFF);
    stack_push(cpu, cpu->Status | FLAG_B | FLAG_U);
    
    cpu->Status |= FLAG_I; // Disable hardware interrupts
    uint16_t low = cpu_read(0xFFFE);
    uint16_t high = cpu_read(0xFFFF);
    cpu->PC = (high << 8) | low;
}

// PHA: Push Accumulator to Stack
void ins_PHA(CPU_6502* cpu, uint16_t addr) {
    stack_push(cpu, cpu->A);
}

// PLA: Pop Accumulator from Stack
void ins_PLA(CPU_6502* cpu, uint16_t addr) {
    cpu->A = stack_pop(cpu);
}

// LDY: load Y register
void ins_LDY(CPU_6502* cpu, uint16_t addr) {
    cpu->Y = cpu_read(addr);
}

//LDA: load A register
void ins_LDA(CPU_6502* cpu, uint16_t addr) {
    cpu->A = cpu_read(addr);
}

// LDX: load X register
void ins_LDX(CPU_6502* cpu, uint16_t addr) {
    cpu->X = cpu_read(addr);
}

//CLC: Clear Carry Bit
void ins_CLC(CPU_6502* cpu, uint16_t addr) {
    cpu->Status &= ~FLAG_C;
}

// STA: store accumulator at address
void ins_STA(CPU_6502* cpu, uint16_t addr) {
    RAM[addr] = cpu->A;
}

// INC: increment memory
void ins_INC(CPU_6502* cpu, uint16_t addr) {
    RAM[addr]++;
}

// ============================================================================
// 5. INSTRUCTION LOOKUP TABLE & EMULATION LOOP
// ============================================================================

typedef struct {
    void (*execute)(CPU_6502*, uint16_t);
    bool (*get_address)(CPU_6502*, uint16_t*);
    uint8_t base_cycles;
} Instruction;

// Define an analytical global opcode matrix
Instruction opcode_matrix[256];

void initialize_opcode_matrix() {
    // Fill matrix with a placeholder for unhandled opcodes
    for (int i = 0; i < 256; i++) {
        opcode_matrix[i].execute = NULL;
    }

    // Register our newly constructed instructions
    opcode_matrix[0x69] = (Instruction){ ins_ADC, mode_IMM, 2 }; // ADC Immediate
    opcode_matrix[0x6D] = (Instruction){ ins_ADC, mode_ABS, 4 }; // ADC Absolute
    opcode_matrix[0x7D] = (Instruction){ ins_ADC, mode_ABX, 4 }; // ADC Absolute,X (Can add +1 cycle)
    opcode_matrix[0x20] = (Instruction){ ins_JSR, mode_ABS, 6 }; // JSR Absolute
    opcode_matrix[0x60] = (Instruction){ ins_RTS, NULL,     6 }; // RTS Implied
    opcode_matrix[0x00] = (Instruction){ ins_BRK, NULL,     7 }; // BRK Implied
    opcode_matrix[0xC9] = (Instruction){ ins_CMP, mode_IMM, 2 }; // CMP Immediate
    opcode_matrix[0xDD] = (Instruction){ ins_CMP, mode_ABX, 4 }; // CMP Immediate
    opcode_matrix[0x48] = (Instruction){ ins_PHA, NULL, 3 }; // PHA (stack push accumulator)
    opcode_matrix[0x68] = (Instruction){ ins_PLA, NULL, 4 }; // PLA (stack pop accumulator)
    opcode_matrix[0xA0] = (Instruction){ ins_LDY, mode_IMM, 2 }; // LDY Immediate
    opcode_matrix[0xB1] = (Instruction){ ins_LDA, mode_IND_Y, 5 }; // LDA indirect Y
    opcode_matrix[0xAD] = (Instruction){ ins_LDA, mode_ABS, 4}; // LDA Absolute
    opcode_matrix[0xBD] = (Instruction){ ins_LDA, mode_ABX, 4}; // LDA Absolute X
    opcode_matrix[0xAE] = (Instruction){ ins_LDX, mode_ABS, 4}; // LDX Absolute
    opcode_matrix[0x18] = (Instruction){ ins_CLC, NULL, 2}; // CLC
    opcode_matrix[0x8D] = (Instruction){ ins_STA, mode_ABS, 4}; // STA Absolute
    opcode_matrix[0x9D] = (Instruction){ ins_STA, mode_ABX, 5}; // STA Absolute X
    opcode_matrix[0xF0] = (Instruction){ ins_BEQ, mode_REL, 2}; // Branch Equal  
    opcode_matrix[0xEE] = (Instruction){ ins_INC, mode_ABS, 3}; // INC Absolute 
    opcode_matrix[0x4C] = (Instruction){ ins_JMP, mode_ABS, 3}; // JMP Absolute 


}

// Master Execution Loop
void step_cpu(CPU_6502* cpu) {
    uint8_t opcode = cpu_read(cpu->PC);
    cpu->PC++;

    Instruction instr = opcode_matrix[opcode];

    if (instr.execute == NULL) {
        printf("Unhandled Opcode: 0x%02X at Address 0x%04X\n", opcode, cpu->PC - 1);
        return;
    }

    uint16_t target_address = 0x0000;
    bool page_crossed = false;

    // Resolve address if the current instruction mode requires an operand location
    if (instr.get_address != NULL) {
        page_crossed = instr.get_address(cpu, &target_address);
    }

    // Execute the underlying operational behavior
    instr.execute(cpu, target_address);

    // Dynamic Cycle Management Calculation
    uint8_t final_cycles = instr.base_cycles;
    if (page_crossed && (opcode == 0x7D)) { // Add extra hardware clock step for ABX page cross
        final_cycles++;
    }
    cpu->Cycles += final_cycles;
}

// ============================================================================
// 6. TESTING HARDWARE ENVIRONMENT CONTEXT
// ============================================================================

int main() {

    // loader
    // loads bytes from a file into the memory
    // start load into address x8000

    size_t program_data_start_address = 0x8000;

    FILE* file = fopen("sampleMachineCode.hex", "rb");

    if(file == NULL)
    {
        perror("error opening file");
        return EXIT_FAILURE;
    }

    int ch;
    size_t offset = 0;

    while((ch = fgetc(file)) != EOF)
    {
        printf("Offset %08zu: Hex: %02X | Char: ", offset, ch);
        RAM[program_data_start_address + offset] = ch;
        offset++;
    }

    fclose(file);

    initialize_opcode_matrix();
    CPU_6502 cpu;

    // Setup Reset Vector targeting execution entry point at 0x8000
    RAM[0xFFFC] = 0x00;
    RAM[0xFFFD] = 0x80;

    // Write a real production assembly tracking program to Memory
    // 0x8000: ADC #$05     -> Add 5 to Accumulator (A becomes 5)
    // 0x8002: JSR $9000    -> Jump to a safe sub-routine at 0x9000
    // 0x9000: ADC #$10     -> Inside subroutine: Add 16 (A becomes 21 / 0x15)
    // 0x9002: RTS          -> Return back safely to main program context
    // 0x8005: BRK          -> End execution safely

    // RAM[0x8000] = 0x69; RAM[0x8001] = 0x05; 
    // RAM[0x8002] = 0x20; RAM[0x8003] = 0x00; RAM[0x8004] = 0x90; 
    // RAM[0x9000] = 0x69; RAM[0x9001] = 0x10; 
    // RAM[0x9002] = 0x60; 
    // RAM[0x8005] = 0x00; 

    // RAM[0x8000] = 0x69; RAM[0x8001] = 0x05; 
    // RAM[0x8002] = 0x69; RAM[0x8003] = 0x03; 
    // RAM[0x8004] = 0x00; 

    reset_cpu(&cpu);
    printf("Initial Reset State -> PC: 0x%04X, SP: 0x%02X, A: 0x%02X\n\n", cpu.PC, cpu.SP, cpu.A);

    // Step through the operations sequentially 
    step_cpu(&cpu); // Runs ADC #$05
    printf("Executed ADC #$05   -> PC: 0x%04X, A: 0x%02X, Accumulated Cycles: %d\n", cpu.PC, cpu.A, cpu.Cycles);

    step_cpu(&cpu); // Runs ADC #$03
    printf("Executed ADC #$03   -> PC: 0x%04X, A: 0x%02X, Accumulated Cycles: %d\n", cpu.PC, cpu.A, cpu.Cycles);

    // step_cpu(&cpu); // Runs JSR $9000
    // printf("Executed ADC #$03   -> PC: 0x%04X, SP: 0x%02X (Pushed to Stack), Accumulated Cycles: %d\n", cpu.PC, cpu.SP, cpu.Cycles);

    // step_cpu(&cpu); // Runs sub-routine ADC #$10
    // printf("Executed Sub-ADC    -> PC: 0x%04X, A: 0x%02X, Accumulated Cycles: %d\n", cpu.PC, cpu.A, cpu.Cycles);

    // step_cpu(&cpu); // Runs RTS
    // printf("Executed RTS        -> PC: 0x%04X, SP: 0x%02X (Popped from Stack), Accumulated Cycles: %d\n", cpu.PC, cpu.SP, cpu.Cycles);

    step_cpu(&cpu); // Runs BRK
    printf("Executed BRK End    -> PC: 0x%04X, Status Flags: 0x%02X, Final System Cycles: %d\n", cpu.PC, cpu.Status, cpu.Cycles);

    return 0;
}