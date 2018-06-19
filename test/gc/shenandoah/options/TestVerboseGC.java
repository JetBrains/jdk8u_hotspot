/*
 * Copyright (c) 2017 Red Hat, Inc. and/or its affiliates.
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
 * @test TestVerboseGC
 * @summary Test that Shenandoah reacts properly on -verbose:gc
 * @key gc
 * @library /testlibrary
 * @modules java.base/jdk.internal.misc
 *          java.management
 * @run driver TestVerboseGC
 */

import com.oracle.java.testlibrary.*;

public class TestVerboseGC {
    static volatile Object sink;

    public static void main(String[] args) throws Exception {
        if (args.length > 0) {
            for (int c = 0; c < 1_000; c++) {
                sink = new byte[1_000_000];
                Thread.sleep(1);
            }
            return;
        }

        {
            ProcessBuilder pb = ProcessTools.createJavaProcessBuilder("-XX:+UseShenandoahGC",
                                                                      "-Xmx128m",
                                                                      TestVerboseGC.class.getName(),
                                                                      "test");
            OutputAnalyzer output = new OutputAnalyzer(pb.start());
            output.shouldNotContain("Concurrent marking");
            output.shouldNotContain("Immediate Garbage");
            output.shouldNotContain("GC STATISTICS");
            output.shouldHaveExitValue(0);
        }

        {
            ProcessBuilder pb = ProcessTools.createJavaProcessBuilder("-XX:+UseShenandoahGC",
                                                                      "-Xmx128m",
                                                                      "-verbose:gc",
                                                                      TestVerboseGC.class.getName(),
                                                                      "test");
            OutputAnalyzer output = new OutputAnalyzer(pb.start());
            output.shouldContain("Concurrent marking");
            output.shouldNotContain("Immediate Garbage");
            output.shouldContain("GC STATISTICS");
            output.shouldHaveExitValue(0);
        }

        {
            ProcessBuilder pb = ProcessTools.createJavaProcessBuilder("-XX:+UseShenandoahGC",
                                                                      "-Xmx128m",
                                                                      "-XX:+PrintGC",
                                                                      TestVerboseGC.class.getName(),
                                                                      "test");
            OutputAnalyzer output = new OutputAnalyzer(pb.start());
            output.shouldContain("Concurrent marking");
            output.shouldNotContain("Immediate Garbage");
            output.shouldContain("GC STATISTICS");
            output.shouldHaveExitValue(0);
        }

        {
            ProcessBuilder pb = ProcessTools.createJavaProcessBuilder("-XX:+UseShenandoahGC",
                                                                      "-Xmx128m",
                                                                      "-XX:+PrintGCDetails",
                                                                      TestVerboseGC.class.getName(),
                                                                      "test");
            OutputAnalyzer output = new OutputAnalyzer(pb.start());
            output.shouldContain("Concurrent marking");
            output.shouldContain("Immediate Garbage");
            output.shouldContain("GC STATISTICS");
            output.shouldHaveExitValue(0);
        }
    }
}
