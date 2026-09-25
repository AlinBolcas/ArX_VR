package com.limelight;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public class ArxWorkspaceTest {
    // Regression: the Mac reported accessibility as 1, the headset read it as false and refused all input
    @Test public void permissionFlagAcceptsBooleanAndNumber() {
        assertTrue(ArxWorkspace.truthy(true));
        assertTrue(ArxWorkspace.truthy(1));
        assertTrue(ArxWorkspace.truthy("true"));
        assertFalse(ArxWorkspace.truthy(false));
        assertFalse(ArxWorkspace.truthy(0));
        assertFalse(ArxWorkspace.truthy(null));
    }
}
