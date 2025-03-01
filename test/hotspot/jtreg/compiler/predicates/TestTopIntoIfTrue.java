/*
 * Copyright (c) 2024, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
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
 * @test
 * @key stress randomness
 * @bug 8342809
 * @summary Test that a top input into an IfTrue of an Assertion Predicate is properly handled during IGVN.
 * @requires vm.compiler2.enabled
 * @run main/othervm -XX:CompileCommand=compileonly,compiler.predicates.TestTopIntoIfTrue::test -XX:-TieredCompilation
 *                   -Xcomp -XX:+UnlockDiagnosticVMOptions -XX:+StressIGVN -XX:StressSeed=1073486978
 *                   compiler.predicates.TestTopIntoIfTrue
 * @run main/othervm -XX:CompileCommand=compileonly,compiler.predicates.TestTopIntoIfTrue::test -XX:-TieredCompilation
 *                   -Xcomp -XX:+UnlockDiagnosticVMOptions -XX:+StressIGVN compiler.predicates.TestTopIntoIfTrue
 */

package compiler.predicates;

public class TestTopIntoIfTrue {
    static int iFld;

    public static void main(String[] strArr) {
        for (int i = 0; i < 100; i++) {
            test();
        }
    }

    static void test() {
        int x = 10;
        for (int i = 1; i < 326; ++i) {
            x += 12;
            if (x != 0) {
                Unloaded.trap(); // unloaded trap
            }
            iFld += 34;
        }
    }
}

class Unloaded {
    static void trap() {}
}