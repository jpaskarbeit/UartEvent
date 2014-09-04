/*
 ||
 || @file 	Serial3Event.cpp
 || @version 	5
 || @author 	Colin Duffy
 || @contact 	cmduffy@engr.psu.edu
 || @license
 || | Copyright (c) 2014 Colin Duffy
 || | This library is free software; you can redistribute it and/or
 || | modify it under the terms of the GNU Lesser General Public
 || | License as published by the Free Software Foundation; version
 || | 2.1 of the License.
 || |
 || | This library is distributed in the hope that it will be useful,
 || | but WITHOUT ANY WARRANTY; without even the implied warranty of
 || | MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 || | Lesser General Public License for more details.
 || |
 || | You should have received a copy of the GNU Lesser General Public
 || | License along with this library; if not, write to the Free Software
 || | Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 || #
 ||
 */

#include "SerialEvent.h"

#define SCGC4_UART2_BIT     12

Serial3Event::ISR       Serial3Event::RX_CALLBACK;
Serial3Event::ISR       Serial3Event::TX_CALLBACK;

event_params_t          Serial3Event::event;
DMAChannel              Serial3Event::tx;
DMAChannel              Serial3Event::rx;

volatile uint8_t        Serial3Event::txUsedMemory;
volatile uint8_t        Serial3Event::rxUsedMemory;
volatile unsigned char  *Serial3Event::rxBuffer;
volatile unsigned char  *Serial3Event::txBuffer;
volatile uint32_t       Serial3Event::rxBufferSize;
// -------------------------------------------ISR------------------------------------------
void Serial3Event::serial_dma_tx_isr( void ) {
    tx.clearInterrupt( );
    int head = event.txHead;
    int tail = event.txTail;
    event.bufTotalSize -= tx.TCD->CITER_ELINKNO;
    if ( (tail + tx.TCD->CITER_ELINKNO) >= event.TX_BUFFER_SIZE ) {
        tail += tx.TCD->CITER_ELINKNO - event.TX_BUFFER_SIZE;
    } else {
        tail += tx.TCD->CITER_ELINKNO;
    }
    //Serial.printf("head: %04i | tail: %04i\n", head, tail);
    if ( head != tail ) {
        event.isTransmitting = true;
        int size;
        if ( tail > head ) {
            size = ( event.TX_BUFFER_SIZE - tail ) + head;
        } else {
            size = head - tail;
        }
        __disable_irq( );
        tx.TCD->CITER = size;
        tx.TCD->BITER = size;
        tx.enable();
        __enable_irq();
    } else {
        event.isTransmitting = false;
        TX_CALLBACK( );
    }
    event.txTail = tail;
    UART2_C2 |= UART_C2_TIE;
}

void Serial3Event::serial_dma_rx_isr( void ) {
    rx.clearInterrupt( );
    if ( event.term_rx_character != -1 ) {
        static uint32_t byteCount_rx = 1;
        rxBufferSize = byteCount_rx;
        if (( ( uint8_t )*event.currentptr_rx == event.term_rx_character) || ( byteCount_rx == event.RX_BUFFER_SIZE ) ) {
            event.currentptr_rx = ( uintptr_t * )rx.TCD->DADDR;
            *event.currentptr_rx = 0;
            rx.TCD->DADDR = event.zeroptr_rx;
            byteCount_rx = 1;
            RX_CALLBACK( );
        }
        else { ++byteCount_rx; }
        event.currentptr_rx = (uintptr_t*)rx.TCD->DADDR;
    }
    else {
        RX_CALLBACK( );
    }
    
}
// -------------------------------------------CODE------------------------------------------
void Serial3Event::serial_dma_begin( uint32_t divisor ) {
    TX_CALLBACK = txEventHandler;
    RX_CALLBACK = rxEventHandler;
    // Enable UART2 clock
    BITBAND_U32( SIM_SCGC4, SCGC4_UART2_BIT ) = 0x01;
    /****************************************************************
     * some code lifted from Teensyduino Core serial1.c
     ****************************************************************/
    CORE_PIN7_CONFIG = PORT_PCR_PE | PORT_PCR_PS | PORT_PCR_PFE | PORT_PCR_MUX(3);
    CORE_PIN8_CONFIG = PORT_PCR_DSE | PORT_PCR_SRE | PORT_PCR_MUX(3);
    UART2_BDH = ( divisor >> 13 ) & 0x1F;
    UART2_BDL = ( divisor >> 5 ) & 0xFF;
    UART2_C4 = divisor & 0x1F;
    UART2_C1 = 0;//UART_C1_ILT;
    // TODO: Use UART2 fifo with dma
    //UART2_TWFIFO = 1; // tx watermark, causes C5_TDMAS DMA request
    //UART2_RWFIFO = 1; // rx watermark, causes C5_RDMAS DMA request
    UART2_PFIFO = 0;//UART_PFIFO_TXFE;
    UART2_C2 = C2_TX_INACTIVE;
    UART2_C5 = UART_DMA_ENABLE; // setup Serial1 tx,rx to use dma
    if ( loopBack ) UART2_C1 |= UART_C1_LOOPS; // Set internal loop1Back
    /****************************************************************
     * DMA TX setup
     ****************************************************************/
    tx.destination( UART2_D );
    tx.sourceCircular(txBuffer, event.TX_BUFFER_SIZE);
    tx.attachInterrupt( serial_dma_tx_isr );
    tx.interruptAtCompletion( );
    tx.disableOnCompletion( );
    tx.triggerAtHardwareEvent( DMAMUX_SOURCE_UART2_TX );
    event.priority = NVIC_GET_PRIORITY(IRQ_DMA_CH0 + tx.channel);
    //int sync_address = ( int )&txBuffer[0] - ( int )tx.TCD->SADDR;
    //Serial.printf("SADDR: %p | txBuffer: %p | sync_address: %X\n\n", tx.TCD->SADDR, &txBuffer[0], sync_address);
    /****************************************************************
     * DMA RX setup
     ****************************************************************/
    if ( rxTermCharacter == -1 && rxTermString == NULL ) {
        rxBufferSize = event.RX_BUFFER_SIZE;
        rx.destinationBuffer( rxBuffer, event.RX_BUFFER_SIZE );
    }
    else {
        rx.destinationCircular(rxBuffer, 1);
        event.term_rx_string = rxTermString;
        event.term_rx_character = rxTermCharacter;
        event.zeroptr_rx = ( uintptr_t * )rx.TCD->DADDR;
        event.currentptr_rx = event.zeroptr_rx;
    }
    rx.source( UART2_D );
    rx.attachInterrupt( serial_dma_rx_isr );
    rx.interruptAtCompletion( );
    rx.triggerContinuously( );
    rx.triggerAtHardwareEvent( DMAMUX_SOURCE_UART2_RX );
    rx.enable( );
}

void Serial3Event::serial_dma_format(uint32_t format) {
    /****************************************************************
     * serial1 format, from teensduino core, serial1.c
     ****************************************************************/
    uint8_t c;
    c = UART2_C1;
    c = ( c & ~0x13 ) | ( format & 0x03 );      // configure parity
    if (format & 0x04) c |= 0x10;           // 9 bits (might include parity)
    UART2_C1 = c;
    if ( ( format & 0x0F ) == 0x04 ) UART2_C3 |= 0x40; // 8N2 is 9 bit with 9th bit always 1
    c = UART2_S2 & ~0x10;
    if ( format & 0x10 ) c |= 0x10;           // rx invert
    UART2_S2 = c;
    c = UART2_C3 & ~0x10;
    if ( format & 0x20 ) c |= 0x10;           // tx invert
    UART2_C3 = c;
}

void Serial3Event::serial_dma_end( void ) {
    if ( !( SIM_SCGC7 & SIM_SCGC7_DMA ) ) return;
    if ( !( SIM_SCGC6 & SIM_SCGC6_DMAMUX ) ) return;
    if ( !( SIM_SCGC4 & SIM_SCGC4_UART2 ) ) return;
    flush();
    delay(20);
    /****************************************************************
     * serial1 end, from teensduino core, serial1.c
     ****************************************************************/
    UART2_C2 = 0;
    CORE_PIN7_CONFIG = PORT_PCR_PE | PORT_PCR_PS | PORT_PCR_MUX(1);
    CORE_PIN8_CONFIG = PORT_PCR_PE | PORT_PCR_PS | PORT_PCR_MUX(1);
    // clear Serial1 dma enable rx/tx bits
    UART2_C5 = UART_DMA_DISABLE;
    event.txHead = event.txTail = 0;
}

void Serial3Event::serial_dma_set_transmit_pin( uint8_t pin ) {
    // TODO: need to update var when finish transmitting serial for RS485
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
    event.transmit_pin = portOutputRegister( pin );
}

void Serial3Event::serial_dma_putchar( uint32_t c ) {
    serial_dma_write( &c, 1 );
}

void Serial3Event::serial_dma_write( const void *buf, unsigned int count ) {
    int head = event.txHead;
    int tail = event.txTail;
    int next = head + count;
    event.bufTotalSize += count;
    
    if (count > event.TX_BUFFER_SIZE) {
        int bufcount = ( count/event.TX_BUFFER_SIZE );
        int bufremainder = count%event.TX_BUFFER_SIZE;
        int halfbuf = event.TX_BUFFER_SIZE;
        flush();
        event.txHead = event.txTail = 0;
        head = 0;
        //TCD->CSR |= DMA_TCD_CSR_INTHALF;
        do {
            tx.TCD->SADDR = &txBuffer[0];
            memcpy_fast( txBuffer, buf+head, event.TX_BUFFER_SIZE );
            
            event.isTransmitting = true;
            __disable_irq();
            tx.TCD->CITER = event.TX_BUFFER_SIZE;
            tx.TCD->BITER = event.TX_BUFFER_SIZE;
            tx.enable();
            __enable_irq();
            
            head += event.TX_BUFFER_SIZE;
            flush();
        } while (--bufcount);
        event.txHead = bufremainder;
        if (bufremainder) {
            tx.TCD->SADDR = &txBuffer[0];
            memcpy_fast( txBuffer, buf+head, bufremainder );
            event.isTransmitting = true;
            __disable_irq();
            tx.TCD->CITER = bufremainder;
            tx.TCD->BITER = bufremainder;
            tx.enable();
            __enable_irq();
            flush();
        }
        tx.TCD->SADDR = &txBuffer[0];
        event.bufTotalSize = 0;
        event.txHead = event.txTail = 0;
        return;
    }
    
    if (event.bufTotalSize > event.TX_BUFFER_SIZE) flush();
    
    bool bufwrap = next >= event.TX_BUFFER_SIZE ? true : false;
    if ( bufwrap ) {
        int over = next - event.TX_BUFFER_SIZE;
        int under = event.TX_BUFFER_SIZE - head;
        if ( under ) memcpy_fast( txBuffer+head, buf, under );
        if ( over ) memcpy_fast( txBuffer, buf+under, over );
        head = over;
    }
    else {
        memcpy_fast( txBuffer+head, buf, count );
        head += count;
    }
    event.txHead = head;

    if ( !event.isTransmitting ) {
        event.isTransmitting = true;
        __disable_irq();
        tx.TCD->CITER = count;
        tx.TCD->BITER = count;
        tx.enable();
        __enable_irq();
    }
}

void Serial3Event::serial_dma_flush( void ) {
    // wait for any remainding dma transfers to complete
    int head = event.txHead;
    int tail = event.txTail;
    raise_priority( );
    while ( head != tail ) {
        head = event.txHead;
        tail = event.txTail;
    }
    while ( event.isTransmitting ) ;
    lower_priority( );
}

int Serial3Event::serial_dma_available( void ) {
    uint32_t head, tail;
    head = event.RX_BUFFER_SIZE - rx.TCD->CITER_ELINKNO;
    tail = event.rxTail;
    if ( head >= tail ) return head - tail;
    return event.RX_BUFFER_SIZE + head - tail;
}

int Serial3Event::serial_dma_getchar( void ) {
    uint32_t head, tail;
    int c;
    head = event.RX_BUFFER_SIZE - rx.TCD->CITER_ELINKNO;
    tail = event.rxTail;
    c = rxBuffer[tail];
    if ( head == tail ) return -1;
    if ( ++tail >= event.RX_BUFFER_SIZE ) tail = 0;
    event.rxTail = tail;
    return c;
}

int Serial3Event::serial_dma_peek( void ) {
    uint32_t head;
    head = event.RX_BUFFER_SIZE - rx.TCD->CITER_ELINKNO;
    return head;
}

void Serial3Event::serial_dma_clear( void ) {
    
}
