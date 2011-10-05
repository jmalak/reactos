/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * PURPOSE:          Keyboard functions
 * FILE:             subsys/win32k/ntuser/keyboard.c
 * PROGRAMERS:       Casper S. Hornstrup (chorns@users.sourceforge.net)
 *                   Rafal Harabien (rafalh@reactos.org)
 */

#include <win32k.h>
DBG_DEFAULT_CHANNEL(UserKbd);

BYTE gKeyStateTable[0x100];
static PKEYBOARD_INDICATOR_TRANSLATION gpKeyboardIndicatorTrans = NULL;

/* FUNCTIONS *****************************************************************/

/*
 * InitKeyboardImpl
 *
 * Initialization -- Right now, just zero the key state
 */
INIT_FUNCTION
NTSTATUS
NTAPI
InitKeyboardImpl(VOID)
{
    RtlZeroMemory(&gKeyStateTable, sizeof(gKeyStateTable));
    return STATUS_SUCCESS;
}

/*
 * IntKeyboardGetIndicatorTrans
 *
 * Asks the keyboard driver to send a small table that shows which
 * lights should connect with which scancodes
 */
static
NTSTATUS APIENTRY
IntKeyboardGetIndicatorTrans(HANDLE hKeyboardDevice,
                             PKEYBOARD_INDICATOR_TRANSLATION *ppIndicatorTrans)
{
    NTSTATUS Status;
    DWORD dwSize = 0;
    IO_STATUS_BLOCK Block;
    PKEYBOARD_INDICATOR_TRANSLATION pRet;

    dwSize = sizeof(KEYBOARD_INDICATOR_TRANSLATION);

    pRet = ExAllocatePoolWithTag(PagedPool,
                                 dwSize,
                                 USERTAG_KBDTABLE);

    while (pRet)
    {
        Status = NtDeviceIoControlFile(hKeyboardDevice,
                                       NULL,
                                       NULL,
                                       NULL,
                                       &Block,
                                       IOCTL_KEYBOARD_QUERY_INDICATOR_TRANSLATION,
                                       NULL, 0,
                                       pRet, dwSize);

        if (Status != STATUS_BUFFER_TOO_SMALL)
            break;

        ExFreePoolWithTag(pRet, USERTAG_KBDTABLE);

        dwSize += sizeof(KEYBOARD_INDICATOR_TRANSLATION);

        pRet = ExAllocatePoolWithTag(PagedPool,
                                     dwSize,
                                     USERTAG_KBDTABLE);
    }

    if (!pRet)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(pRet, USERTAG_KBDTABLE);
        return Status;
    }

    *ppIndicatorTrans = pRet;
    return Status;
}

/*
 * IntKeyboardUpdateLeds
 *
 * Sends the keyboard commands to turn on/off the lights
 */
static
NTSTATUS APIENTRY
IntKeyboardUpdateLeds(HANDLE hKeyboardDevice,
                      WORD wScanCode,
                      BOOL bEnabled,
                      PKEYBOARD_INDICATOR_TRANSLATION pIndicatorTrans)
{
    NTSTATUS Status;
    UINT i;
    static KEYBOARD_INDICATOR_PARAMETERS Indicators;
    IO_STATUS_BLOCK Block;

    if (!pIndicatorTrans)
        return STATUS_NOT_SUPPORTED;

    for (i = 0; i < pIndicatorTrans->NumberOfIndicatorKeys; i++)
    {
        if (pIndicatorTrans->IndicatorList[i].MakeCode == wScanCode)
        {
            if (bEnabled)
                Indicators.LedFlags |= pIndicatorTrans->IndicatorList[i].IndicatorFlags;
            else
                Indicators.LedFlags = ~pIndicatorTrans->IndicatorList[i].IndicatorFlags;

            /* Update the lights on the hardware */
            Status = NtDeviceIoControlFile(hKeyboardDevice,
                                           NULL,
                                           NULL,
                                           NULL,
                                           &Block,
                                           IOCTL_KEYBOARD_SET_INDICATORS,
                                           &Indicators, sizeof(Indicators),
                                           NULL, 0);

            return Status;
        }
    }

    return STATUS_SUCCESS;
}

/*
 * IntSimplifyVk
 *
 * Changes virtual keys which distinguish between left and right hand, to keys which don't distinguish
 */
static
WORD
IntSimplifyVk(WORD wVk)
{
    switch (wVk)
    {
        case VK_LSHIFT:
        case VK_RSHIFT:
            return VK_SHIFT;

        case VK_LCONTROL:
        case VK_RCONTROL:
            return VK_CONTROL;

        case VK_LMENU:
        case VK_RMENU:
            return VK_MENU;

        default:
            return wVk;
    }
}

/*
 * IntFixVk
 *
 * Changes virtual keys which don't not distinguish between left and right hand to proper keys
 */
static
WORD
IntFixVk(WORD wVk, BOOL bExt)
{
    switch (wVk)
    {
        case VK_SHIFT:
            return bExt ? VK_RSHIFT : VK_LSHIFT;

        case VK_CONTROL:
            return bExt ? VK_RCONTROL : VK_LCONTROL;

        case VK_MENU:
            return bExt ? VK_RMENU : VK_LMENU;

        default:
            return wVk;
    }
}

/*
 * IntGetVkOtherSide
 *
 * Gets other side of vk: right -> left, left -> right
 */
static
WORD
IntGetVkOtherSide(WORD wVk)
{
    switch (wVk)
    {
        case VK_LSHIFT: return VK_RSHIFT;
        case VK_RSHIFT: return VK_LSHIFT;

        case VK_LCONTROL: return VK_RCONTROL;
        case VK_RCONTROL: return VK_LCONTROL;

        case VK_LMENU: return VK_RMENU;
        case VK_RMENU: return VK_LMENU;

        default: return wVk;
    }
}

/*
 * IntTranslateNumpadKey
 *
 * Translates numpad keys when numlock is enabled
 */
static
WORD
IntTranslateNumpadKey(WORD wVk)
{
    switch (wVk)
    {
        case VK_INSERT: return VK_NUMPAD0;
        case VK_END: return VK_NUMPAD1;
        case VK_DOWN: return VK_NUMPAD2;
        case VK_NEXT: return VK_NUMPAD3;
        case VK_LEFT: return VK_NUMPAD4;
        case VK_CLEAR: return VK_NUMPAD5;
        case VK_RIGHT: return VK_NUMPAD6;
        case VK_HOME: return VK_NUMPAD7;
        case VK_UP: return VK_NUMPAD8;
        case VK_PRIOR: return VK_NUMPAD9;
        case VK_DELETE: return VK_DECIMAL;
        default: return wVk;
    }
}

/*
 * IntGetModifiers
 *
 * Returns a value that indicates if the key is a modifier key, and
 * which one.
 */
static
UINT FASTCALL
IntGetModifiers(PBYTE pKeyState)
{
    UINT fModifiers = 0;
    
    if (pKeyState[VK_SHIFT] & KS_DOWN_BIT)
        fModifiers |= MOD_SHIFT;

    if (pKeyState[VK_CONTROL] & KS_DOWN_BIT)
        fModifiers |= MOD_CONTROL;

    if (pKeyState[VK_MENU] & KS_DOWN_BIT)
        fModifiers |= MOD_ALT;

    if ((pKeyState[VK_LWIN] | pKeyState[VK_RWIN]) & KS_DOWN_BIT)
        fModifiers |= MOD_WIN;

    return fModifiers;
}

/*
 * IntGetShiftBit
 *
 * Search the keyboard layout modifiers table for the shift bit
 */
static
DWORD FASTCALL
IntGetShiftBit(PKBDTABLES pKbdTbl, WORD wVk)
{
    unsigned i;

    for (i = 0; pKbdTbl->pCharModifiers->pVkToBit[i].Vk; i++)
        if (pKbdTbl->pCharModifiers->pVkToBit[i].Vk == wVk)
            return pKbdTbl->pCharModifiers->pVkToBit[i].ModBits;

    return 0;
}

/*
 * IntGetModBits
 *
 * Gets layout specific modification bits, for example KBDSHIFT, KBDCTRL, KBDALT
 */
static
DWORD
IntGetModBits(PKBDTABLES pKbdTbl, PBYTE pKeyState)
{
    DWORD i, dwModBits = 0;

    /* DumpKeyState( KeyState ); */

    for (i = 0; pKbdTbl->pCharModifiers->pVkToBit[i].Vk; i++)
        if (pKeyState[pKbdTbl->pCharModifiers->pVkToBit[i].Vk] & KS_DOWN_BIT)
            dwModBits |= pKbdTbl->pCharModifiers->pVkToBit[i].ModBits;

    /* Handle Alt+Gr */
    if ((pKbdTbl->fLocaleFlags & KLLF_ALTGR) && (pKeyState[VK_RMENU] & KS_DOWN_BIT))
        dwModBits |= IntGetShiftBit(pKbdTbl, VK_CONTROL); /* Don't use KBDCTRL here */

    TRACE("Current Mod Bits: %lx\n", dwModBits);

    return dwModBits;
}

/*
 * IntTranslateChar
 *
 * Translates virtual key to character
 */
static
BOOL
IntTranslateChar(WORD wVirtKey,
                 PBYTE pKeyState,
                 PBOOL pbDead,
                 PBOOL pbLigature,
                 PWCHAR pwcTranslatedChar,
                 PKBDTABLES pKbdTbl)
{
    PVK_TO_WCHAR_TABLE pVkToVchTbl;
    PVK_TO_WCHARS10 pVkToVch;
    DWORD i, dwModBits, dwVkModBits, dwModNumber = 0;
    WCHAR wch;
    BOOL bAltGr;
    WORD wCaplokAttr;

    dwModBits = pKeyState ? IntGetModBits(pKbdTbl, pKeyState) : 0;
    bAltGr = pKeyState && (pKbdTbl->fLocaleFlags & KLLF_ALTGR) && (pKeyState[VK_RMENU] & KS_DOWN_BIT);
    wCaplokAttr = bAltGr ? CAPLOKALTGR : CAPLOK;

    TRACE("TryToTranslate: %04x %x\n", wVirtKey, dwModBits);

    if (dwModBits > pKbdTbl->pCharModifiers->wMaxModBits)
    {
        TRACE("dwModBits %x > wMaxModBits %x\n", dwModBits, pKbdTbl->pCharModifiers->wMaxModBits);
        return FALSE;
    }

    for (i = 0; pKbdTbl->pVkToWcharTable[i].pVkToWchars; i++)
    {
        pVkToVchTbl = &pKbdTbl->pVkToWcharTable[i];
        pVkToVch = (PVK_TO_WCHARS10)(pVkToVchTbl->pVkToWchars);
        while (pVkToVch->VirtualKey)
        {
            if (wVirtKey == (pVkToVch->VirtualKey & 0xFF))
            {
                dwVkModBits = dwModBits;

                /* If CapsLock is enabled for this key and locked, add SHIFT bit */
                if ((pVkToVch->Attributes & wCaplokAttr) &&
                    pKeyState &&
                    (pKeyState[VK_CAPITAL] & KS_LOCK_BIT))
                {
                    /* Note: we use special value here instead of getting VK_SHIFT mod bit - it's verified */
                    dwVkModBits ^= KBDSHIFT;
                }

                /* If ALT without CTRL has ben used, remove ALT flag */
                if ((dwVkModBits & (KBDALT|KBDCTRL)) == KBDALT)
                    dwVkModBits &= ~KBDALT;

                if (dwVkModBits > pKbdTbl->pCharModifiers->wMaxModBits)
                    break;

                /* Get modification number */
                dwModNumber = pKbdTbl->pCharModifiers->ModNumber[dwVkModBits];
                if (dwModNumber >= pVkToVchTbl->nModifications)
                {
                    TRACE("dwModNumber %u >= nModifications %u\n", dwModNumber, pVkToVchTbl->nModifications);
                    break;
                }

                /* Read character */
                wch = pVkToVch->wch[dwModNumber];
                if (wch == WCH_NONE)
                    break;

                *pbDead = (wch == WCH_DEAD);
                *pbLigature = (wch == WCH_LGTR);
                *pwcTranslatedChar = wch;

                TRACE("%d %04x: dwModNumber %08x Char %04x\n",
                      i, wVirtKey, dwModNumber, wch);

                if (*pbDead)
                {
                    /* After WCH_DEAD, real character is located */
                    pVkToVch = (PVK_TO_WCHARS10)(((BYTE *)pVkToVch) + pVkToVchTbl->cbSize);
                    if (pVkToVch->VirtualKey != 0xFF)
                    {
                        WARN("Found dead key with no trailer in the table.\n");
                        WARN("VK: %04x, ADDR: %p\n", wVirtKey, pVkToVch);
                        break;
                    }
                    *pwcTranslatedChar = pVkToVch->wch[dwModNumber];
                }
                return TRUE;
            }
            pVkToVch = (PVK_TO_WCHARS10)(((BYTE *)pVkToVch) + pVkToVchTbl->cbSize);
        }
    }

    /* If nothing has been found in layout, check if this is ASCII control character.
       Note: we could add it to layout table, but windows does not have it there */
    if (wVirtKey >= 'A' && wVirtKey <= 'Z' &&
        (pKeyState[VK_CONTROL] & KS_DOWN_BIT) &&
        !(pKeyState[VK_MENU] & KS_DOWN_BIT))
    {
        *pwcTranslatedChar = (wVirtKey - 'A') + 1; /* ASCII control character */
        *pbDead = FALSE;
        *pbLigature = FALSE;
        return TRUE;
    }

    return FALSE;
}

/*
 * IntToUnicodeEx
 *
 * Translates virtual key to characters
 */
static
int APIENTRY
IntToUnicodeEx(UINT wVirtKey,
               UINT wScanCode,
               PBYTE pKeyState,
               LPWSTR pwszBuff,
               int cchBuff,
               UINT wFlags,
               PKBDTABLES pKbdTbl)
{
    WCHAR wchTranslatedChar;
    BOOL bDead, bLigature;
    static WCHAR wchDead = 0;
    int iRet = 0;

    ASSERT(pKbdTbl);

    if (!IntTranslateChar(wVirtKey,
                          pKeyState,
                          &bDead,
                          &bLigature,
                          &wchTranslatedChar,
                          pKbdTbl))
    {
        return 0;
    }

    if (bLigature)
    {
        WARN("Not handling ligature (yet)\n" );
        return 0;
    }

    /* If we got dead char in previous call check dead keys in keyboard layout */
    if (wchDead)
    {
        UINT i;
        WCHAR wchFirst, wchSecond;
        TRACE("Previous dead char: %lc (%x)\n", wchDead, wchDead);

        for (i = 0; pKbdTbl->pDeadKey[i].dwBoth; i++)
        {
            wchFirst = pKbdTbl->pDeadKey[i].dwBoth >> 16;
            wchSecond = pKbdTbl->pDeadKey[i].dwBoth & 0xFFFF;
            if (wchFirst == wchDead && wchSecond == wchTranslatedChar)
            {
                wchTranslatedChar = pKbdTbl->pDeadKey[i].wchComposed;
                wchDead = 0;
                bDead = FALSE;
                break;
            }
        }

        TRACE("Final char: %lc (%x)\n", wchTranslatedChar, wchTranslatedChar);
    }

    /* dead char has not been not found */
    if (wchDead)
    {
        /* Treat both characters normally */
        if (cchBuff > iRet)
            pwszBuff[iRet++] = wchDead;
        bDead = FALSE;
    }

    /* add character to the buffer */
    if (cchBuff > iRet)
        pwszBuff[iRet++] = wchTranslatedChar;

    /* Save dead character */
    wchDead = bDead ? wchTranslatedChar : 0;

    return bDead ? -iRet : iRet;
}

/*
 * IntVkToVsc
 *
 * Translates virtual key to scan code
 */
static
WORD FASTCALL
IntVkToVsc(WORD wVk, PKBDTABLES pKbdTbl)
{
    unsigned i;

    ASSERT(pKbdTbl);

    /* Check standard keys first */
    for (i = 0; i < pKbdTbl->bMaxVSCtoVK; i++)
    {
        if ((pKbdTbl->pusVSCtoVK[i] & 0xFF) == wVk)
            return i;
    }

    /* Check extended keys now */
    for (i = 0; pKbdTbl->pVSCtoVK_E0[i].Vsc; i++)
    {
        if ((pKbdTbl->pVSCtoVK_E0[i].Vk & 0xFF) == wVk)
            return 0xE000 | pKbdTbl->pVSCtoVK_E0[i].Vsc;
    }

    for (i = 0; pKbdTbl->pVSCtoVK_E1[i].Vsc; i++)
    {
        if ((pKbdTbl->pVSCtoVK_E1[i].Vk & 0xFF) == wVk)
            return 0xE100 | pKbdTbl->pVSCtoVK_E1[i].Vsc;
    }

    /* Virtual key has not been found */
    return 0;
}

/*
 * IntVscToVk
 *
 * Translates prefixed scancode to virtual key
 */
static
WORD FASTCALL
IntVscToVk(WORD wScanCode, PKBDTABLES pKbdTbl)
{
    unsigned i;
    WORD wVk = 0;

    ASSERT(pKbdTbl);

    if ((wScanCode & 0xFF00) == 0xE000)
    {
        for (i = 0; pKbdTbl->pVSCtoVK_E0[i].Vsc; i++)
        {
            if (pKbdTbl->pVSCtoVK_E0[i].Vsc == (wScanCode & 0xFF))
            {
                wVk = pKbdTbl->pVSCtoVK_E0[i].Vk;
            }
        }
    }
    else if ((wScanCode & 0xFF00) == 0xE100)
    {
        for (i = 0; pKbdTbl->pVSCtoVK_E1[i].Vsc; i++)
        {
            if (pKbdTbl->pVSCtoVK_E1[i].Vsc == (wScanCode & 0xFF))
            {
                wVk = pKbdTbl->pVSCtoVK_E1[i].Vk;
            }
        }
    }
    else if (wScanCode < pKbdTbl->bMaxVSCtoVK)
    {
        wVk = pKbdTbl->pusVSCtoVK[wScanCode];
    }

    /* 0xFF nad 0x00 are invalid VKs */
    return wVk != 0xFF ? wVk : 0;
}

/*
 * IntVkToChar
 *
 * Translates virtual key to character, ignoring shift state
 */
static
WCHAR FASTCALL
IntVkToChar(WORD wVk, PKBDTABLES pKbdTbl)
{
    WCHAR wch;
    BOOL bDead, bLigature;

    ASSERT(pKbdTbl);

    if (IntTranslateChar(wVk,
                         NULL,
                         &bDead,
                         &bLigature,
                         &wch,
                         pKbdTbl))
    {
        return wch;
    }

    return 0;
}

#if 0
static
VOID
DumpKeyState(PBYTE pKeyState)
{
    unsigned i;

    DbgPrint("KeyState { ");
    for (i = 0; i < 0x100; i++)
    {
        if (pKeyState[i])
            DbgPrint("%02x(%02x) ", i, pKeyState[i]);
    }
    DbgPrint("};\n");
}
#endif

/*
 * IntGetAsyncKeyState
 *
 * Gets key state from global table
 */
static
WORD FASTCALL
IntGetAsyncKeyState(DWORD dwKey)
{
    WORD dwRet = 0;

    if (dwKey < 0x100)
    {
        if (gKeyStateTable[dwKey] & KS_DOWN_BIT)
            dwRet |= 0xFFFF8000; // If down, windows returns 0xFFFF8000.
        if (gKeyStateTable[dwKey] & KS_LOCK_BIT)
            dwRet |= 0x1;
    }
    else
    {
        EngSetLastError(ERROR_INVALID_PARAMETER);
    }
    return dwRet;
}

SHORT
APIENTRY
NtUserGetAsyncKeyState(INT Key)
{
    DECLARE_RETURN(SHORT);

    TRACE("Enter NtUserGetAsyncKeyState\n");
    UserEnterExclusive();

    RETURN(IntGetAsyncKeyState(Key));

CLEANUP:
    TRACE("Leave NtUserGetAsyncKeyState, ret=%i\n", _ret_);
    UserLeave();
    END_CLEANUP;
}

/*
 * IntKeyboardSendWinKeyMsg
 *
 * Sends syscommand to shell, when WIN key is pressed
 */
static
VOID NTAPI
IntKeyboardSendWinKeyMsg()
{
    PWND pWnd;
    MSG Msg;

    if (!(pWnd = UserGetWindowObject(InputWindowStation->ShellWindow)))
    {
        ERR("Couldn't find window to send Windows key message!\n");
        return;
    }

    Msg.hwnd = InputWindowStation->ShellWindow;
    Msg.message = WM_SYSCOMMAND;
    Msg.wParam = SC_TASKLIST;
    Msg.lParam = 0;

    /* The QS_HOTKEY is just a guess */
    MsqPostMessage(pWnd->head.pti->MessageQueue, &Msg, FALSE, QS_HOTKEY);
}

/*
 * co_IntKeyboardSendAltKeyMsg
 *
 * Sends syscommand enabling window menu
 */
static
VOID NTAPI
co_IntKeyboardSendAltKeyMsg()
{
    FIXME("co_IntKeyboardSendAltKeyMsg\n");
    //co_MsqPostKeyboardMessage(WM_SYSCOMMAND,SC_KEYMENU,0); // This sends everything into a msg loop!
}

/*
 * UserSendKeyboardInput
 *
 * Process keyboard input from input devices and SendInput API
 */
BOOL NTAPI
UserSendKeyboardInput(KEYBDINPUT *pKbdInput, BOOL bInjected)
{
    WORD wScanCode, wVk, wSimpleVk, wVkOtherSide, wSysKey;
    PKBL pKbl = NULL;
    PKBDTABLES pKbdTbl;
    PUSER_MESSAGE_QUEUE pFocusQueue;
    struct _ETHREAD *pFocusThread;
    UINT fModifiers;
    BYTE PrevKeyState = 0;
    HWND hWnd;
    int HotkeyId;
    struct _ETHREAD *Thread;
    LARGE_INTEGER LargeTickCount;
    BOOL bExt = pKbdInput->dwFlags & KEYEVENTF_EXTENDEDKEY;
    BOOL bKeyUp = pKbdInput->dwFlags & KEYEVENTF_KEYUP;

    /* Find the target thread whose locale is in effect */
    pFocusQueue = IntGetFocusMessageQueue();

    if (pFocusQueue)
    {
        pFocusThread = pFocusQueue->Thread;
        if (pFocusThread && pFocusThread->Tcb.Win32Thread)
            pKbl = ((PTHREADINFO)pFocusThread->Tcb.Win32Thread)->KeyboardLayout;
    }

    if (!pKbl)
        pKbl = W32kGetDefaultKeyLayout();
    if (!pKbl)
    {
        ERR("No keyboard layout!\n");
        return FALSE;
    }

    pKbdTbl = pKbl->KBTables;

    /* Note: wScan field is always used */
    wScanCode = pKbdInput->wScan & 0x7F;

    if (pKbdInput->dwFlags & KEYEVENTF_UNICODE)
    {
        /* Generate WM_KEYDOWN msg with wParam == VK_PACKET and
           high order word of lParam == pKbdInput->wScan */
        wVk = VK_PACKET;
    }
    else
    {
        if (bExt)
            wScanCode |= 0xE000;

        if (pKbdInput->dwFlags & KEYEVENTF_SCANCODE)
        {
            /* Don't ignore invalid scan codes */
            wVk = IntVscToVk(wScanCode, pKbdTbl);
            if (!wVk) /* use 0xFF if vsc is invalid */
                wVk = 0xFF;
        }
        else
        {
            wVk = pKbdInput->wVk & 0xFF;

            /* LSHIFT + EXT = RSHIFT */
            wVk = IntSimplifyVk(wVk);
            wVk = IntFixVk(wVk, bExt);
        }
    }

    if (!(pKbdInput->dwFlags & KEYEVENTF_UNICODE))
    {
        /* Get virtual key without shifts (VK_(L|R)* -> VK_*) */
        wSimpleVk = IntSimplifyVk(wVk);
        wVkOtherSide = IntGetVkOtherSide(wVk);
        PrevKeyState = gKeyStateTable[wSimpleVk];

        /* Update global keyboard state. Begin from lock bit */
        if (!bKeyUp && !(PrevKeyState & KS_DOWN_BIT))
            gKeyStateTable[wVk] ^= KS_LOCK_BIT;

        /* Update down bit */
        if (bKeyUp)
            gKeyStateTable[wVk] &= ~KS_DOWN_BIT;
        else
            gKeyStateTable[wVk] |= KS_DOWN_BIT;

        /* Update key without shifts */
        gKeyStateTable[wSimpleVk] = gKeyStateTable[wVk] | gKeyStateTable[wVkOtherSide];

        if (!bKeyUp)
        {
            /* Update keyboard LEDs */
            if (!gpKeyboardIndicatorTrans)
                IntKeyboardGetIndicatorTrans(ghKeyboardDevice, &gpKeyboardIndicatorTrans);
            if (gpKeyboardIndicatorTrans)
                IntKeyboardUpdateLeds(ghKeyboardDevice,
                                      wScanCode,
                                      gKeyStateTable[wSimpleVk] & KS_LOCK_BIT,
                                      gpKeyboardIndicatorTrans);
        }

        /* Truncate scan code */
        wScanCode &= 0x7F;

        /* Support VK_*WIN and VK_*MENU keys */
        if (bKeyUp)
        {
            if (wVk == VK_LWIN || wVk == VK_RWIN)
                IntKeyboardSendWinKeyMsg();
            else if(wSimpleVk == VK_MENU && !(gKeyStateTable[VK_CONTROL] & KS_DOWN_BIT))
                co_IntKeyboardSendAltKeyMsg();
        }

        /* Check if it is a hotkey */
        fModifiers = IntGetModifiers(gKeyStateTable);
        if (GetHotKey(fModifiers, wSimpleVk, &Thread, &hWnd, &HotkeyId))
        {
            if (!bKeyUp)
            {
                TRACE("Hot key pressed (hWnd %lx, id %d)\n", hWnd, HotkeyId);
                MsqPostHotKeyMessage(Thread,
                                     hWnd,
                                     (WPARAM)HotkeyId,
                                     MAKELPARAM((WORD)fModifiers, wSimpleVk));
            }

            return TRUE; /* Don't send any message */
        }
    }

    /* If we have a focus queue, post a keyboard message */
    if (pFocusQueue)
    {
        MSG Msg;

        /* If it is F10 or ALT is down and CTRL is up, it's a system key */
        wSysKey = (pKbdTbl->fLocaleFlags & KLLF_ALTGR) ? VK_LMENU : VK_MENU;
        if (wVk == VK_F10 ||
            //uVkNoShift == VK_MENU || // FIXME: If only LALT is pressed WM_SYSKEYUP is generated instead of WM_KEYUP
            ((gKeyStateTable[wSysKey] & KS_DOWN_BIT) && // FIXME
            !(gKeyStateTable[VK_CONTROL] & KS_DOWN_BIT)))
        {
            if (bKeyUp)
                Msg.message = WM_SYSKEYUP;
            else
                Msg.message = WM_SYSKEYDOWN;
        }
        else
        {
            if (bKeyUp)
                Msg.message = WM_KEYUP;
            else
                Msg.message = WM_KEYDOWN;
        }
        Msg.hwnd = pFocusQueue->FocusWindow;
        Msg.lParam = MAKELPARAM(1, wScanCode);
        /* If it is VK_PACKET, high word of wParam is used for wchar */
        if (!(pKbdInput->dwFlags & KEYEVENTF_UNICODE))
        {
            if (bExt)
                Msg.lParam |= LP_EXT_BIT;
            if (gKeyStateTable[VK_MENU] & KS_DOWN_BIT)
                Msg.lParam |= LP_CONTEXT_BIT;
            if (PrevKeyState & KS_DOWN_BIT)
                Msg.lParam |= LP_PREV_STATE_BIT;
            if (bKeyUp)
                Msg.lParam |= LP_TRANSITION_BIT;
        }

        Msg.wParam = wVk; // its "simplified" later
        Msg.pt = gpsi->ptCursor;
        if (pKbdInput->time)
            Msg.time = pKbdInput->time;
        else
        {
            KeQueryTickCount(&LargeTickCount);
            Msg.time = MsqCalculateMessageTime(&LargeTickCount);
        }

        /* Post a keyboard message */
        TRACE("Posting keyboard msg %u wParam 0x%x lParam 0x%x\n", Msg.message, Msg.wParam, Msg.lParam);
        co_MsqPostKeyboardMessage(Msg.message, Msg.wParam, Msg.lParam, bInjected);
    }

    return TRUE;
}

/* 
 * UserProcessKeyboardInput
 *
 * Process raw keyboard input data
 */
VOID NTAPI
UserProcessKeyboardInput(
    PKEYBOARD_INPUT_DATA pKbdInputData)
{
    WORD wScanCode, wVk;
    PKBL pKbl = NULL;
    PKBDTABLES pKbdTbl;
    PUSER_MESSAGE_QUEUE pFocusQueue;
    struct _ETHREAD *pFocusThread;

    /* Calculate scan code with prefix */
    wScanCode = pKbdInputData->MakeCode & 0x7F;
    if (pKbdInputData->Flags & KEY_E0)
        wScanCode |= 0xE000;
    if (pKbdInputData->Flags & KEY_E1)
        wScanCode |= 0xE100;

    /* Find the target thread whose locale is in effect */
    pFocusQueue = IntGetFocusMessageQueue();

    if (pFocusQueue)
    {
        pFocusThread = pFocusQueue->Thread;
        if (pFocusThread && pFocusThread->Tcb.Win32Thread)
            pKbl = ((PTHREADINFO)pFocusThread->Tcb.Win32Thread)->KeyboardLayout;
    }

    if (!pKbl)
        pKbl = W32kGetDefaultKeyLayout();
    if (!pKbl)
        return;

    pKbdTbl = pKbl->KBTables;

    /* Convert scan code to virtual key.
       Note: we could call UserSendKeyboardInput using scan code,
             but it wouldn't interpret E1 key(s) properly */
    wVk = IntVscToVk(wScanCode, pKbdTbl);
    TRACE("UserProcessKeyboardInput: %x (break: %u) -> %x\n",
          wScanCode, (pKbdInputData->Flags & KEY_BREAK) ? 1 : 0, wVk);

    if (wVk)
    {
        KEYBDINPUT KbdInput;

        /* Support numlock */
        if ((wVk & KBDNUMPAD) && (gKeyStateTable[VK_NUMLOCK] & KS_LOCK_BIT))
        {
            wVk = IntTranslateNumpadKey(wVk & 0xFF);
        }

        /* Send keyboard input */
        KbdInput.wVk = wVk & 0xFF;
        KbdInput.wScan = wScanCode & 0x7F;
        KbdInput.dwFlags = 0;
        if (pKbdInputData->Flags & KEY_BREAK)
            KbdInput.dwFlags |= KEYEVENTF_KEYUP;
        if (wVk & KBDEXT)
            KbdInput.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        KbdInput.time = 0;
        KbdInput.dwExtraInfo = pKbdInputData->ExtraInformation;
        UserSendKeyboardInput(&KbdInput, FALSE);

        /* E1 keys don't have break code */
        if (pKbdInputData->Flags & KEY_E1)
        {
            /* Send key up event */
            KbdInput.dwFlags |= KEYEVENTF_KEYUP;
            UserSendKeyboardInput(&KbdInput, FALSE);
        }
    }
}

/* 
 * IntTranslateKbdMessage
 *
 * Addes WM_(SYS)CHAR messages to message queue if message
 * describes key which produce character.
 */
BOOL FASTCALL
IntTranslateKbdMessage(LPMSG lpMsg,
                       UINT flags)
{
    PTHREADINFO pti;
    INT cch = 0, i;
    WCHAR wch[3] = { 0 };
    MSG NewMsg = { 0 };
    PKBDTABLES pKbdTbl;
    PWND pWnd;
    LARGE_INTEGER LargeTickCount;
    BOOL bResult = FALSE;

    pWnd = UserGetWindowObject(lpMsg->hwnd);
    if (!pWnd) // Must have a window!
    {
        ERR("No Window for Translate.\n");
        return FALSE;
    }

    pti = pWnd->head.pti;
    pKbdTbl = pti->KeyboardLayout->KBTables;
    if (!pKbdTbl)
        return FALSE;

    if (lpMsg->message != WM_KEYDOWN && lpMsg->message != WM_SYSKEYDOWN)
        return FALSE;

    /* Init pt, hwnd and time msg fields */
    NewMsg.pt = gpsi->ptCursor;
    NewMsg.hwnd = lpMsg->hwnd;
    KeQueryTickCount(&LargeTickCount);
    NewMsg.time = MsqCalculateMessageTime(&LargeTickCount);

    TRACE("Enter IntTranslateKbdMessage msg %s, vk %x\n",
        lpMsg->message == WM_SYSKEYDOWN ? "WM_SYSKEYDOWN" : "WM_KEYDOWN", lpMsg->wParam);

    if (lpMsg->wParam == VK_PACKET)
    {
        NewMsg.message = (lpMsg->message == WM_KEYDOWN) ? WM_CHAR : WM_SYSCHAR;
        NewMsg.wParam = HIWORD(lpMsg->lParam);
        NewMsg.lParam = LOWORD(lpMsg->lParam);
        MsqPostMessage(pti->MessageQueue, &NewMsg, FALSE, QS_KEY);
        return TRUE;
    }

    cch = IntToUnicodeEx(lpMsg->wParam,
                         HIWORD(lpMsg->lParam) & 0xFF,
                         pti->MessageQueue->KeyState,
                         wch,
                         sizeof(wch) / sizeof(wch[0]),
                         0,
                         pKbdTbl);

    if (cch)
    {
        if (cch > 0) /* Normal characters */
            NewMsg.message = (lpMsg->message == WM_KEYDOWN) ? WM_CHAR : WM_SYSCHAR;
        else /* Dead character */
        {
            cch = -cch;
            NewMsg.message =
                (lpMsg->message == WM_KEYDOWN) ? WM_DEADCHAR : WM_SYSDEADCHAR;
        }
        NewMsg.lParam = lpMsg->lParam;

        /* Send all characters */
        for (i = 0; i < cch; ++i)
        {
            TRACE("Msg: %x '%lc' (%04x) %08x\n", NewMsg.message, wch[i], wch[i], NewMsg.lParam);
            NewMsg.wParam = wch[i];
            MsqPostMessage(pti->MessageQueue, &NewMsg, FALSE, QS_KEY);
        }
        bResult = TRUE;
    }

    TRACE("Leave IntTranslateKbdMessage ret %u, cch %d, msg %x, wch %x\n",
        bResult, cch, NewMsg.message, NewMsg.wParam);
    return bResult;
}

/*
 * Map a virtual key code, or virtual scan code, to a scan code, key code,
 * or unshifted unicode character.
 *
 * Code: See Below
 * Type:
 * 0 -- Code is a virtual key code that is converted into a virtual scan code
 *      that does not distinguish between left and right shift keys.
 * 1 -- Code is a virtual scan code that is converted into a virtual key code
 *      that does not distinguish between left and right shift keys.
 * 2 -- Code is a virtual key code that is converted into an unshifted unicode
 *      character.
 * 3 -- Code is a virtual scan code that is converted into a virtual key code
 *      that distinguishes left and right shift keys.
 * KeyLayout: Keyboard layout handle
 *
 * @implemented
 */
static UINT
IntMapVirtualKeyEx(UINT uCode, UINT Type, PKBDTABLES pKbdTbl)
{
    UINT uRet = 0;

    switch (Type)
    {
        case MAPVK_VK_TO_VSC:
            uCode = IntFixVk(uCode, FALSE);
            uRet = IntVkToVsc(uCode, pKbdTbl);
            if (uRet > 0xFF) // fail for scancodes with prefix (e0, e1)
                uRet = 0;
            break;

        case MAPVK_VSC_TO_VK:
            uRet = IntVscToVk(uCode, pKbdTbl) & 0xFF;
            uRet = IntSimplifyVk(uRet);
            break;

        case MAPVK_VK_TO_CHAR:
            uRet = (UINT)IntVkToChar(uCode, pKbdTbl);
        break;

        case MAPVK_VSC_TO_VK_EX:
            uRet = IntVscToVk(uCode, pKbdTbl) & 0xFF;
            break;

        case MAPVK_VK_TO_VSC_EX:
            uRet = IntVkToVsc(uCode, pKbdTbl);
            break;

        default:
            EngSetLastError(ERROR_INVALID_PARAMETER);
            ERR("Wrong type value: %u\n", Type);
    }

    return uRet;
}

/*
 * NtUserMapVirtualKeyEx
 *
 * Map a virtual key code, or virtual scan code, to a scan code, key code,
 * or unshifted unicode character. See IntMapVirtualKeyEx.
 */
UINT
APIENTRY
NtUserMapVirtualKeyEx(UINT uCode, UINT uType, DWORD keyboardId, HKL dwhkl)
{
    PKBDTABLES pKbdTbl = NULL;
    DECLARE_RETURN(UINT);

    TRACE("Enter NtUserMapVirtualKeyEx\n");
    UserEnterExclusive();

    if (!dwhkl)
    {
        PTHREADINFO pti;

        pti = PsGetCurrentThreadWin32Thread();
        if (pti && pti->KeyboardLayout)
            pKbdTbl = pti->KeyboardLayout->KBTables;
    }
    else
    {
        PKBL pKbl;

        pKbl = UserHklToKbl(dwhkl);
        if (pKbl)
            pKbdTbl = pKbl->KBTables;
    }

    if (!pKbdTbl)
        RETURN(0);

    RETURN(IntMapVirtualKeyEx(uCode, uType, pKbdTbl));

CLEANUP:
    TRACE("Leave NtUserMapVirtualKeyEx, ret=%i\n", _ret_);
    UserLeave();
    END_CLEANUP;
}

/*
 * NtUserToUnicodeEx
 *
 * Translates virtual key to characters
 */
int
APIENTRY
NtUserToUnicodeEx(
    UINT wVirtKey,
    UINT wScanCode,
    PBYTE pKeyStateUnsafe,
    LPWSTR pwszBuffUnsafe,
    int cchBuff,
    UINT wFlags,
    HKL dwhkl)
{
    PTHREADINFO pti;
    BYTE KeyState[0x100];
    PWCHAR pwszBuff = NULL;
    int iRet = 0;
    PKBL pKbl = NULL;
    DECLARE_RETURN(int);

    TRACE("Enter NtUserSetKeyboardState\n");
    UserEnterShared();

    /* Key up? */
    if (wScanCode & SC_KEY_UP)
    {
        RETURN(0);
    }

    if (!NT_SUCCESS(MmCopyFromCaller(KeyState,
                                     pKeyStateUnsafe,
                                     sizeof(KeyState))))
    {
        ERR("Couldn't copy key state from caller.\n");
        RETURN(0);
    }

    /* Virtual code is correct? */
    if (wVirtKey < 0x100)
    {
        pwszBuff = ExAllocatePoolWithTag(NonPagedPool, sizeof(WCHAR) * cchBuff, TAG_STRING);
        if (!pwszBuff)
        {
            ERR("ExAllocatePoolWithTag(%d) failed\n", sizeof(WCHAR) * cchBuff);
            RETURN(0);
        }
        RtlZeroMemory(pwszBuff, sizeof(WCHAR) * cchBuff);

        if (dwhkl)
            pKbl = UserHklToKbl(dwhkl);

        if (!pKbl)
        {
            pti = PsGetCurrentThreadWin32Thread();
            pKbl = pti->KeyboardLayout;
        }

        iRet = IntToUnicodeEx(wVirtKey,
                              wScanCode,
                              KeyState,
                              pwszBuff,
                              cchBuff,
                              wFlags,
                              pKbl ? pKbl->KBTables : NULL);

        MmCopyToCaller(pwszBuffUnsafe, pwszBuff, cchBuff * sizeof(WCHAR));
        ExFreePoolWithTag(pwszBuff, TAG_STRING);
    }

    RETURN(iRet);

CLEANUP:
    TRACE("Leave NtUserSetKeyboardState, ret=%i\n", _ret_);
    UserLeave();
    END_CLEANUP;
}

/*
 * NtUserGetKeyNameText
 *
 * Gets key name from keyboard layout
 */
DWORD
APIENTRY
NtUserGetKeyNameText(LONG lParam, LPWSTR lpString, int cchSize)
{
    PTHREADINFO pti;
    DWORD i, cchKeyName, dwRet = 0;
    WORD wScanCode = (lParam >> 16) & 0xFF;
    BOOL bExtKey = (lParam & LP_EXT_BIT) ? TRUE : FALSE;
    PKBDTABLES pKbdTbl;
    VSC_LPWSTR *pKeyNames = NULL;
    CONST WCHAR *pKeyName = NULL;
    WCHAR KeyNameBuf[2];
    DECLARE_RETURN(DWORD);

    TRACE("Enter NtUserGetKeyNameText\n");
    UserEnterShared();

    /* Get current keyboard layout */
    pti = PsGetCurrentThreadWin32Thread();
    pKbdTbl = pti ? pti->KeyboardLayout->KBTables : 0;

    if (!pKbdTbl || cchSize < 1)
        RETURN(0);

    /* "Do not care" flag */
    if(lParam & LP_DO_NOT_CARE_BIT)
    {
        /* Note: we could do vsc -> vk -> vsc conversion, instead of using
                 hardcoded scan codes, but it's not what Windows does */
        if (wScanCode == SCANCODE_RSHIFT && !bExtKey)
            wScanCode = SCANCODE_LSHIFT;
        else if (wScanCode == SCANCODE_CTRL || wScanCode == SCANCODE_ALT)
            bExtKey = FALSE;
    }

    if (bExtKey)
        pKeyNames = pKbdTbl->pKeyNamesExt;
    else
        pKeyNames = pKbdTbl->pKeyNames;

    for (i = 0; pKeyNames[i].pwsz; i++)
    {
        if (pKeyNames[i].vsc == wScanCode)
        {
            pKeyName = pKeyNames[i].pwsz;
            break;
        }
    }

    if (!pKeyName)
    {
        WORD wVk = IntVscToVk(wScanCode, pKbdTbl);

        if (wVk)
        {
            KeyNameBuf[0] = IntVkToChar(wVk, pKbdTbl);
            KeyNameBuf[1] = 0;
            if (KeyNameBuf[0])
                pKeyName = KeyNameBuf;
        }
    }

    if (pKeyName)
    {
        cchKeyName = wcslen(pKeyName);
        if (cchKeyName > cchSize - 1)
            cchKeyName = cchSize - 1; // don't count '\0'

        _SEH2_TRY
        {
            ProbeForWrite(lpString, (cchKeyName + 1) * sizeof(WCHAR), 1);
            RtlCopyMemory(lpString, pKeyName, cchKeyName * sizeof(WCHAR));
            lpString[cchKeyName] = UNICODE_NULL;
            dwRet = cchKeyName;
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            SetLastNtError(_SEH2_GetExceptionCode());
        }
        _SEH2_END;
    }
    else
    {
        EngSetLastError(ERROR_INVALID_PARAMETER);
    }

    RETURN(dwRet);

CLEANUP:
    TRACE("Leave NtUserGetKeyNameText, ret=%i\n", _ret_);
    UserLeave();
    END_CLEANUP;
}

/*
 * UserGetKeyboardType
 *
 * Returns some keyboard specific information
 */
DWORD FASTCALL
UserGetKeyboardType(
    DWORD dwTypeFlag)
{
    switch (dwTypeFlag)
    {
        case 0:        /* Keyboard type */
            return 4;    /* AT-101 */
        case 1:        /* Keyboard Subtype */
            return 0;    /* There are no defined subtypes */
        case 2:        /* Number of F-keys */
            return 12;   /* We're doing an 101 for now, so return 12 F-keys */
        default:
            ERR("Unknown type!\n");
            return 0;    /* Note: we don't have to set last error here */
    }
}

/*
 * NtUserVkKeyScanEx
 *
 * Based on IntTranslateChar, instead of processing VirtualKey match,
 * look for wChar match.
 */
DWORD
APIENTRY
NtUserVkKeyScanEx(
    WCHAR wch,
    HKL dwhkl,
    BOOL bUsehKL)
{
    PKBDTABLES pKbdTbl;
    PVK_TO_WCHAR_TABLE pVkToWchTbl;
    PVK_TO_WCHARS10 pVkToWch;
    PKBL pKbl = NULL;
    DWORD i, dwModBits = 0, dwModNumber = 0, Ret = (DWORD)-1;

    TRACE("NtUserVkKeyScanEx() wch %d, KbdLayout 0x%p\n", wch, dwhkl);
    UserEnterShared();

    if (bUsehKL)
    {
        // Use given keyboard layout
        if (dwhkl)
            pKbl = UserHklToKbl(dwhkl);
    }
    else
    {
        // Use thread keyboard layout
        pKbl = ((PTHREADINFO)PsGetCurrentThreadWin32Thread())->KeyboardLayout;
    }

    if (!pKbl)
        goto Exit;

    pKbdTbl = pKbl->KBTables;

    // Interate through all VkToWchar tables while pVkToWchars is not NULL
    for (i = 0; pKbdTbl->pVkToWcharTable[i].pVkToWchars; i++)
    {
        pVkToWchTbl = &pKbdTbl->pVkToWcharTable[i];
        pVkToWch = (PVK_TO_WCHARS10)(pVkToWchTbl->pVkToWchars);

        // Interate through all virtual keys
        while (pVkToWch->VirtualKey)
        {
            for (dwModNumber = 0; dwModNumber < pVkToWchTbl->nModifications; dwModNumber++)
            {
                if (pVkToWch->wch[dwModNumber] == wch)
                {
                    dwModBits = pKbdTbl->pCharModifiers->ModNumber[dwModNumber];
                    TRACE("i %d wC %04x: dwModBits %08x dwModNumber %08x MaxModBits %08x\n",
                          i, wch, dwModBits, dwModNumber, pKbdTbl->pCharModifiers->wMaxModBits);
                    Ret = (dwModBits << 8) | (pVkToWch->VirtualKey & 0xFF);
                    goto Exit;
                }
            }
            pVkToWch = (PVK_TO_WCHARS10)(((BYTE *)pVkToWch) + pVkToWchTbl->cbSize);
        }
    }
Exit:
    UserLeave();
    return Ret;
}

/*
 * UserGetMouseButtonsState
 *
 * Returns bitfield used in mouse messages
 */
WORD FASTCALL
UserGetMouseButtonsState(VOID)
{
    WORD ret = 0;

    if (gpsi->aiSysMet[SM_SWAPBUTTON])
    {
        if (gKeyStateTable[VK_RBUTTON] & KS_DOWN_BIT) ret |= MK_LBUTTON;
        if (gKeyStateTable[VK_LBUTTON] & KS_DOWN_BIT) ret |= MK_RBUTTON;
    }
    else
    {
        if (gKeyStateTable[VK_LBUTTON] & KS_DOWN_BIT) ret |= MK_LBUTTON;
        if (gKeyStateTable[VK_RBUTTON] & KS_DOWN_BIT) ret |= MK_RBUTTON;
    }
    if (gKeyStateTable[VK_MBUTTON]  & KS_DOWN_BIT) ret |= MK_MBUTTON;
    if (gKeyStateTable[VK_SHIFT]    & KS_DOWN_BIT) ret |= MK_SHIFT;
    if (gKeyStateTable[VK_CONTROL]  & KS_DOWN_BIT) ret |= MK_CONTROL;
    if (gKeyStateTable[VK_XBUTTON1] & KS_DOWN_BIT) ret |= MK_XBUTTON1;
    if (gKeyStateTable[VK_XBUTTON2] & KS_DOWN_BIT) ret |= MK_XBUTTON2;
    return ret;
}

/* EOF */
