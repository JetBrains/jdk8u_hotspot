/*
 * Copyright (c) 2016, Red Hat, Inc. and/or its affiliates.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */
/*
 * @test TestHumongous
 * @summary test reclaim of humongous object
 * @run main/othervm -Xint -XX:+UseShenandoahGC -XX:ShenandoahHeapRegionSize=1M TestHumongous
 */

public class TestHumongous {
    // Shenandoah heap memory region size = 1 M
    private static int M = 1024 * 1024;
    public static void main(String[] args) {
        // Construct a humongous object (oop) just fits multiple regions
        // 8 bytes for object header, 8 bytes for brooks pointer
        int size = 2 * M  - 8 - 8;
        // The oop spans 2 regions
        char[] ch = new char[size];
        for (int index = 0; index < 10; index ++) {
            ch[index] = 'A';
        }

        System.out.print(ch[1]);
        ch = null;
        // Force a GC to clean up above object
        System.gc();
    }
}

