/**
 * HardFault handler для nRF52840 (T114)
 * Дамп регистров при критическом сбое для диагностики зависаний
 */

#include <Arduino.h>
#include <nrfx.h>

// Отключаем C++ name mangling для C-функций прерываний
extern "C" {

// Прототип C-функции обработчика
void HardFault_Handler_C(uint32_t* sp) __attribute__((naked));
void HardFault_Handler(void) __attribute__((naked));

// C-часть обработчика — дамп регистров
void HardFault_Handler_C(uint32_t* sp) {
  // Быстрая инициализация Serial (если ещё не был)
  Serial.begin(115200);
  delay(100);  // Даём UART стабилизироваться
  
  Serial.println("\n");
  Serial.println("!!! ===================================== !!!");
  Serial.println("!!!          HARDFAULT DETECTED           !!!");
  Serial.println("!!! ===================================== !!!");
  Serial.printf("Timestamp: %lu ms\n", (unsigned long)millis());
  Serial.println();
  
  // Основные регистры
  Serial.println("=== General Purpose Registers ===");
  Serial.printf("R0  = 0x%08lX\n", sp[0]);
  Serial.printf("R1  = 0x%08lX\n", sp[1]);
  Serial.printf("R2  = 0x%08lX\n", sp[2]);
  Serial.printf("R3  = 0x%08lX\n", sp[3]);
  Serial.printf("R4  = 0x%08lX\n", sp[4]);
  Serial.printf("R5  = 0x%08lX\n", sp[5]);
  Serial.printf("R6  = 0x%08lX\n", sp[6]);
  Serial.printf("R7  = 0x%08lX\n", sp[7]);
  Serial.printf("R8  = 0x%08lX\n", sp[8]);
  Serial.printf("R9  = 0x%08lX\n", sp[9]);
  Serial.printf("R10 = 0x%08lX\n", sp[10]);
  Serial.printf("R11 = 0x%08lX\n", sp[11]);
  Serial.printf("R12 = 0x%08lX\n", sp[12]);
  Serial.println();
  
  // Критические регистры
  Serial.println("=== Critical Registers ===");
  Serial.printf("LR  = 0x%08lX\n", sp[13]);
  Serial.printf("PC  = 0x%08lX\n", sp[14]);
  Serial.printf("PSR = 0x%08lX\n", sp[15]);
  Serial.println();
  
  // Регистры состояния Fault
  Serial.println("=== Fault Status Registers ===");
  Serial.printf("BFAR = 0x%08lX (Bad Fault Address)\n", (unsigned long)SCB->BFAR);
  Serial.printf("CFSR = 0x%08lX (Configurable Fault Status)\n", (unsigned long)SCB->CFSR);
  Serial.printf("HFSR = 0x%08lX (Hard Fault Status)\n", (unsigned long)SCB->HFSR);
  Serial.printf("DFSR = 0x%08lX (Debug Fault Status)\n", (unsigned long)SCB->DFSR);
  Serial.printf("AFSR = 0x%08lX (Auxiliary Fault Status)\n", (unsigned long)SCB->AFSR);
  Serial.println();
  
  // Расшифровка CFSR
  Serial.println("=== CFSR Breakdown ===");
  uint32_t cfsr = SCB->CFSR;
  
  // UFSR (Usage Fault Status) - биты 15:3
  if (cfsr & (1UL << 3)) Serial.println("  [UFSR] NOCP: No Coprocessor");
  if (cfsr & (1UL << 4)) Serial.println("  [UFSR] UNALIGNED: Unaligned access");
  if (cfsr & (1UL << 5)) Serial.println("  [UFSR] DIVBYZERO: Divide by zero");
  if (cfsr & (1UL << 6)) Serial.println("  [UFSR] STKERR: Stack error");
  if (cfsr & (1UL << 7)) Serial.println("  [UFSR] MSTERR: Memory management error");
  if (cfsr & (1UL << 8)) Serial.println("  [UFSR] IBUSERR: Instruction bus error");
  if (cfsr & (1UL << 9)) Serial.println("  [UFSR] PRECISERR: Precise data bus error");
  if (cfsr & (1UL << 10)) Serial.println("  [UFSR] IMPRECISERR: Imprecise data bus error");
  if (cfsr & (1UL << 11)) Serial.println("  [UFSR] UNSTKERR: Unstacking error");
  if (cfsr & (1UL << 12)) Serial.println("  [UFSR] STKERR: Stacking error");
  if (cfsr & (1UL << 13)) Serial.println("  [UFSR] MMARVALID: MMAR contains valid address");
  
  // HFSR биты
  Serial.println();
  Serial.println("=== HFSR Breakdown ===");
  uint32_t hfsr = SCB->HFSR;
  if (hfsr & (1UL << 1)) Serial.println("  VECTTBL: Vector table read error");
  if (hfsr & (1UL << 30)) Serial.println("  FORCED: Forced HardFault (escalated fault)");
  if (hfsr & (1UL << 31)) Serial.println("  DEBUGEVT: Debug event");
  
  // Информация о стеке
  Serial.println();
  Serial.println("=== Stack Information ===");
  Serial.printf("MSP (Main Stack Pointer)     = 0x%08lX\n", __get_MSP());
  Serial.printf("PSP (Process Stack Pointer)  = 0x%08lX\n", __get_PSP());
  Serial.printf("Current Stack (by EXC_RETURN)= %s\n", (sp[13] & (1UL << 2)) ? "PSP" : "MSP");
  
  // Информация о куче
  Serial.println();
  Serial.println("=== Heap Information ===");
  Serial.printf("Free Heap Size: %u bytes\n", (unsigned)xPortGetFreeHeapSize());
  
  // Информация о SoftDevice (если доступна)
  Serial.println();
  Serial.println("=== System Information ===");
  Serial.printf("CPU Frequency: %lu MHz\n", (unsigned long)(F_CPU / 1000000UL));
  Serial.printf("Uptime: %lu seconds\n", (unsigned long)(millis() / 1000UL));
  
  Serial.println();
  Serial.println("!!! ===================================== !!!");
  Serial.println("!!!         SYSTEM HALTED - REBOOT        !!!");
  Serial.println("!!! ===================================== !!!");
  Serial.flush();
  
  // Даём время на передачу данных по UART
  delay(500);
  
  // Глубокий сон для сохранения логов (если подключён отладчик)
  // Или программный сброс через watchdog
  NRF_POWER->SYSTEMOFF = 1;
  
  // Бесконечный цикл на случай если SYSTEMOFF не сработал
  while (1) {
    __WFI();  // Wait For Interrupt
  }
}

// Ассемблерная обёртка — определяет какой стек использовался
void HardFault_Handler(void) {
  __asm volatile (
    ".syntax unified\n"
    "movs r0, #4\n"
    "movs r1, lr\n"
    "tst r0, r1\n"
    "beq _MSP\n"
    "mrs r0, psp\n"
    "b _done\n"
    "_MSP:\n"
    "mrs r0, msp\n"
    "_done:\n"
    "push {lr}\n"
    "bl HardFault_Handler_C\n"
    "pop {pc}\n"
    ".syntax divided\n"
  );
}

// Обработчик прерывания MemManage (опционально)
void MemoryManagement_Handler(void) __attribute__((weak, alias("HardFault_Handler")));

// Обработчик прерывания BusFault (опционально)
void BusFault_Handler(void) __attribute__((weak, alias("HardFault_Handler")));

// Обработчик прерывания UsageFault (опционально)
void UsageFault_Handler(void) __attribute__((weak, alias("HardFault_Handler")));

}  // extern "C"
