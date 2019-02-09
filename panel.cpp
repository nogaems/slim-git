/* SLiM - Simple Login Manager
   Copyright (C) 1997, 1998 Per Liden
   Copyright (C) 2004-06 Simone Rota <sip@varlock.com>
   Copyright (C) 2004-06 Johannes Winkelmann <jw@tks6.net>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
*/

#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <array>
#include <chrono>
#include <sstream>
#include <poll.h>
#include <X11/extensions/Xrandr.h>
#include "panel.h"

using namespace std;

Panel::Panel(Display* dpy, int scr, Window root, Cfg* config, const string& themedir, PanelType panel_mode)
    : Dpy(dpy), Scr(scr), Root(root), cfg(config), mode(panel_mode), session_name(""), session_exec(""),
      // Load properties from config / theme
      input_name(cfg->getIntOption("input_name_x"), cfg->getIntOption("input_name_y")),
      input_pass(cfg->getIntOption("input_pass_x"), cfg->getIntOption("input_pass_y")),
      inputShadowOffset(cfg->getIntOption("input_shadow_xoffset"), cfg->getIntOption("input_shadow_yoffset")),
      text_widget(cfg->getIntOption("text_widget_x"), cfg->getIntOption("text_widget_y")),
      text_widget_shadow_offset(cfg->getIntOption("text_widget_shadow_xoffset"),
                                cfg->getIntOption("text_widget_shadow_yoffset"))
{
    if (mode == Mode_Lock) {
        Win = root;
        viewport = GetPrimaryViewport();
    }

    // Init GC
    XGCValues gcv;
    unsigned long gcm = GCForeground | GCBackground | GCGraphicsExposures;
    gcv.foreground = GetColor("black");
    gcv.background = GetColor("white");
    gcv.graphics_exposures = False;
    if (mode == Mode_Lock) {
        TextGC = XCreateGC(Dpy, Win, gcm, &gcv);
        gcm = GCGraphicsExposures;
        gcv.graphics_exposures = False;
        WinGC = XCreateGC(Dpy, Win, gcm, &gcv);
        if (WinGC < 0) {
            cerr << APPNAME << ": failed to create pixmap\n.";
            exit(ERR_EXIT);
        }
    } else {
        TextGC = XCreateGC(Dpy, Root, gcm, &gcv);
    }

    font = XftFontOpenName(Dpy, Scr, cfg->getOption("input_font").c_str());
    welcomefont = XftFontOpenName(Dpy, Scr, cfg->getOption("welcome_font").c_str());
    introfont = XftFontOpenName(Dpy, Scr, cfg->getOption("intro_font").c_str());
    enterfont = XftFontOpenName(Dpy, Scr, cfg->getOption("username_font").c_str());
    msgfont = XftFontOpenName(Dpy, Scr, cfg->getOption("msg_font").c_str());
    text_widget_font = XftFontOpenName(Dpy, Scr, cfg->getOption("text_widget_font").c_str());

    Visual* visual = DefaultVisual(Dpy, Scr);
    Colormap colormap = DefaultColormap(Dpy, Scr);
    // NOTE: using XftColorAllocValue() would be a better solution. Lazy me.
    XftColorAllocName(Dpy, visual, colormap, cfg->getOption("input_color").c_str(), &inputcolor);
    XftColorAllocName(Dpy, visual, colormap, cfg->getOption("input_shadow_color").c_str(), &inputshadowcolor);
    XftColorAllocName(Dpy, visual, colormap, cfg->getOption("welcome_color").c_str(), &welcomecolor);
    XftColorAllocName(Dpy, visual, colormap, cfg->getOption("welcome_shadow_color").c_str(), &welcomeshadowcolor);
    XftColorAllocName(Dpy, visual, colormap, cfg->getOption("username_color").c_str(), &entercolor);
    XftColorAllocName(Dpy, visual, colormap, cfg->getOption("username_shadow_color").c_str(), &entershadowcolor);
    XftColorAllocName(Dpy, visual, colormap, cfg->getOption("msg_color").c_str(), &msgcolor);
    XftColorAllocName(Dpy, visual, colormap, cfg->getOption("msg_shadow_color").c_str(), &msgshadowcolor);
    XftColorAllocName(Dpy, visual, colormap, cfg->getOption("intro_color").c_str(), &introcolor);
    XftColorAllocName(Dpy, DefaultVisual(Dpy, Scr), colormap,
                      cfg->getOption("session_color").c_str(), &sessioncolor);
    XftColorAllocName(Dpy, DefaultVisual(Dpy, Scr), colormap,
                      cfg->getOption("session_shadow_color").c_str(), &sessionshadowcolor);
    XftColorAllocName(Dpy, visual, colormap, cfg->getOption("text_widget_color").c_str(), &text_widget_color);
    XftColorAllocName(Dpy, visual, colormap, cfg->getOption("text_widget_shadow_color").c_str(),
                      &text_widget_shadow_color);

    if (input_pass.x < 0 || input_pass.y < 0) { // single inputbox mode
        input_pass.x = input_name.x;
        input_pass.y = input_name.y;
    }

    text_widget_former_string = "";
    text_widget_command = cfg->getOption("text_widget_command").c_str();
    text_widget_interval = std::atof(cfg->getOption("text_widget_interval").c_str());

    // Load panel and background image
    string panelpng = "";
    panelpng = panelpng + themedir +"/panel.png";
    image = new Image;
    bool loaded = image->Read(panelpng.c_str());
    if (!loaded) { // try jpeg if png failed
        panelpng = themedir + "/panel.jpg";
        loaded = image->Read(panelpng.c_str());
        if (!loaded) {
            logStream << APPNAME
                 << ": could not load panel image for theme '"
                 << basename((char*)themedir.c_str()) << "'"
                 << endl;
            exit(ERR_EXIT);
        }
    }

    Image* bg = new Image();
    string bgstyle = cfg->getOption("background_style");
    if (bgstyle != "color") {
        panelpng = themedir +"/background.png";
        loaded = bg->Read(panelpng.c_str());
        if (!loaded) { // try jpeg if png failed
            panelpng = themedir + "/background.jpg";
            loaded = bg->Read(panelpng.c_str());
            if (!loaded){
                logStream << APPNAME
                     << ": could not load background image for theme '"
                     << basename((char*)themedir.c_str()) << "'"
                     << endl;
                exit(ERR_EXIT);
            }
        }
    }

    if (mode == Mode_Lock) {
        if (bgstyle == "stretch") {
            bg->Resize(viewport.width, viewport.height);
            //bg->Resize(XWidthOfScreen(ScreenOfDisplay(Dpy, Scr)),
            //                      XHeightOfScreen(ScreenOfDisplay(Dpy, Scr)));
        } else if (bgstyle == "tile") {
            bg->Tile(viewport.width, viewport.height);
        } else if (bgstyle == "center") {
            string hexvalue = cfg->getOption("background_color");
            hexvalue = hexvalue.substr(1,6);
            bg->Center(viewport.width,
                       viewport.height,
                       hexvalue.c_str());
        } else { // plain color or error
            string hexvalue = cfg->getOption("background_color");
            hexvalue = hexvalue.substr(1,6);
            bg->Center(viewport.width,
                       viewport.height,
                       hexvalue.c_str());
        }
    } else {
        if (bgstyle == "stretch") {
            bg->Resize(XWidthOfScreen(ScreenOfDisplay(Dpy, Scr)), XHeightOfScreen(ScreenOfDisplay(Dpy, Scr)));
        } else if (bgstyle == "tile") {
            bg->Tile(XWidthOfScreen(ScreenOfDisplay(Dpy, Scr)), XHeightOfScreen(ScreenOfDisplay(Dpy, Scr)));
        } else if (bgstyle == "center") {
            string hexvalue = cfg->getOption("background_color");
            hexvalue = hexvalue.substr(1,6);
            bg->Center(XWidthOfScreen(ScreenOfDisplay(Dpy, Scr)),
                       XHeightOfScreen(ScreenOfDisplay(Dpy, Scr)),
                       hexvalue.c_str());
        } else { // plain color or error
            string hexvalue = cfg->getOption("background_color");
            hexvalue = hexvalue.substr(1,6);
            bg->Center(XWidthOfScreen(ScreenOfDisplay(Dpy, Scr)),
                       XHeightOfScreen(ScreenOfDisplay(Dpy, Scr)),
                       hexvalue.c_str());
        }
    }

    string cfgX = cfg->getOption("input_panel_x");
    string cfgY = cfg->getOption("input_panel_y");

    if (mode == Mode_Lock) {
        X = Cfg::absolutepos(cfgX, viewport.width, image->Width());
        Y = Cfg::absolutepos(cfgY, viewport.height, image->Height());

        input_name.x += X;
        input_name.y += Y;
        input_pass.x += X;
        input_pass.y += Y;

        // Merge image into background without crop
        image->Merge_non_crop(bg, X, Y);
        PanelPixmap = image->createPixmap(Dpy, Scr, Win);
    } else {
        X = Cfg::absolutepos(cfgX, XWidthOfScreen(ScreenOfDisplay(Dpy, Scr)), image->Width());
        Y = Cfg::absolutepos(cfgY, XHeightOfScreen(ScreenOfDisplay(Dpy, Scr)), image->Height());

        // Merge image into background
        image->Merge(bg, X, Y);
        PanelPixmap = image->createPixmap(Dpy, Scr, Root);
    }
    delete bg;

    // Read (and substitute vars in) the welcome message
    welcome_message = cfg->getWelcomeMessage();
    intro_message = cfg->getOption("intro_msg");

    if (mode == Mode_Lock) {
        SetName(getenv("USER"));
        field = Get_Passwd;
        OnExpose();
    }
}

Panel::~Panel() {
    XftColorFree (Dpy, DefaultVisual(Dpy, Scr), DefaultColormap(Dpy, Scr), &inputcolor);
    XftColorFree (Dpy, DefaultVisual(Dpy, Scr), DefaultColormap(Dpy, Scr), &inputshadowcolor);
    XftColorFree (Dpy, DefaultVisual(Dpy, Scr), DefaultColormap(Dpy, Scr), &welcomecolor);
    XftColorFree (Dpy, DefaultVisual(Dpy, Scr), DefaultColormap(Dpy, Scr), &welcomeshadowcolor);
    XftColorFree (Dpy, DefaultVisual(Dpy, Scr), DefaultColormap(Dpy, Scr), &entercolor);
    XftColorFree (Dpy, DefaultVisual(Dpy, Scr), DefaultColormap(Dpy, Scr), &entershadowcolor);
    XftColorFree (Dpy, DefaultVisual(Dpy, Scr), DefaultColormap(Dpy, Scr), &msgcolor);
    XftColorFree (Dpy, DefaultVisual(Dpy, Scr), DefaultColormap(Dpy, Scr), &msgshadowcolor);
    XftColorFree (Dpy, DefaultVisual(Dpy, Scr), DefaultColormap(Dpy, Scr), &introcolor);
    XftColorFree (Dpy, DefaultVisual(Dpy, Scr), DefaultColormap(Dpy, Scr), &sessioncolor);
    XftColorFree (Dpy, DefaultVisual(Dpy, Scr), DefaultColormap(Dpy, Scr), &sessionshadowcolor);
    XftColorFree (Dpy, DefaultVisual(Dpy, Scr), DefaultColormap(Dpy, Scr), &text_widget_color);
    XftColorFree (Dpy, DefaultVisual(Dpy, Scr), DefaultColormap(Dpy, Scr), &text_widget_shadow_color);
    XFreeGC(Dpy, TextGC);
    XftFontClose(Dpy, font);
    XftFontClose(Dpy, msgfont);
    XftFontClose(Dpy, introfont);
    XftFontClose(Dpy, welcomefont);
    XftFontClose(Dpy, enterfont);
    XftFontClose(Dpy, text_widget_font);

    if (mode == Mode_Lock) {
        XFreeGC(Dpy, WinGC);
    }

    delete image;
}

void Panel::OpenPanel() {
    // Create window
    Win = XCreateSimpleWindow(Dpy, Root, X, Y,
                              image->Width(),
                              image->Height(),
                              0, GetColor("white"), GetColor("white"));

    // Events
    XSelectInput(Dpy, Win, ExposureMask | KeyPressMask);

    // Set background
    XSetWindowBackgroundPixmap(Dpy, Win, PanelPixmap);

    // Show window
    XMapWindow(Dpy, Win);
    XMoveWindow(Dpy, Win, X, Y); // override wm positioning (for tests)

    // Grab keyboard
    XGrabKeyboard(Dpy, Win, False, GrabModeAsync, GrabModeAsync, CurrentTime);

    XFlush(Dpy);

}

void Panel::ClosePanel() {
    XUngrabKeyboard(Dpy, CurrentTime);
    XUnmapWindow(Dpy, Win);
    XDestroyWindow(Dpy, Win);
    XFlush(Dpy);
}

void Panel::ClearPanel() {
    session_name = "";
    session_exec = "";
    Reset();
    XClearWindow(Dpy, Root);
    XClearWindow(Dpy, Win);
    Cursor(SHOW);
    ShowText();
    XFlush(Dpy);
}

void Panel::WrongPassword(int timeout) {
    string message;
    XGlyphInfo extents;

    /*
    if (CapsLockOn)
        message = cfg->getOption("passwd_feedback_capslock");
    else */
    message = cfg->getOption("passwd_feedback_msg");

    XftDraw *draw = XftDrawCreate(Dpy, Win,
        DefaultVisual(Dpy, Scr), DefaultColormap(Dpy, Scr));
        XftTextExtents8(Dpy, msgfont, reinterpret_cast<const XftChar8*>(message.c_str()),
        message.length(), &extents);

    string cfgX = cfg->getOption("passwd_feedback_x");
    string cfgY = cfg->getOption("passwd_feedback_y");
    int shadowXOffset = cfg->getIntOption("msg_shadow_xoffset");
    int shadowYOffset = cfg->getIntOption("msg_shadow_yoffset");
    int msg_x = Cfg::absolutepos(cfgX, XWidthOfScreen(ScreenOfDisplay(Dpy, Scr)), extents.width);
    int msg_y = Cfg::absolutepos(cfgY, XHeightOfScreen(ScreenOfDisplay(Dpy, Scr)), extents.height);

    OnExpose();
    SlimDrawString8(draw, &msgcolor, msgfont, msg_x, msg_y, message,
        &msgshadowcolor, shadowXOffset, shadowYOffset);

    if (cfg->getOption("bell") == "1")
        XBell(Dpy, 100);

    XFlush(Dpy);
    sleep(timeout);
    ResetPasswd();
    OnExpose();
    // The message should stay on the screen even after the password field is
    // cleared, methinks. I don't like this solution, but it works.
    SlimDrawString8(draw, &msgcolor, msgfont, msg_x, msg_y, message,
        &msgshadowcolor, shadowXOffset, shadowYOffset);
    XSync(Dpy, True);
    XftDrawDestroy(draw);
}

void Panel::Message(const string& text) {
    string cfgX, cfgY;
    XGlyphInfo extents;
    XftDraw *draw;
    if (mode == Mode_Lock) {
        draw = XftDrawCreate(Dpy, Win, DefaultVisual(Dpy, Scr), DefaultColormap(Dpy, Scr));
    } else {
        draw = XftDrawCreate(Dpy, Root, DefaultVisual(Dpy, Scr), DefaultColormap(Dpy, Scr));
    }
    XftTextExtentsUtf8(Dpy, msgfont, reinterpret_cast<const XftChar8*>(text.c_str()),
                    text.length(), &extents);
    cfgX = cfg->getOption("msg_x");
    cfgY = cfg->getOption("msg_y");
    int shadowXOffset = cfg->getIntOption("msg_shadow_xoffset");
    int shadowYOffset = cfg->getIntOption("msg_shadow_yoffset");
    int msg_x, msg_y;
    if (mode == Mode_Lock) {
        msg_x = Cfg::absolutepos(cfgX, viewport.width, extents.width);
        msg_y = Cfg::absolutepos(cfgY, viewport.height, extents.height);
    } else {
        msg_x = Cfg::absolutepos(cfgX, XWidthOfScreen(ScreenOfDisplay(Dpy, Scr)), extents.width);
        msg_y = Cfg::absolutepos(cfgY, XHeightOfScreen(ScreenOfDisplay(Dpy, Scr)), extents.height);
    }
    SlimDrawString8 (draw, &msgcolor, msgfont, msg_x, msg_y,
                     text,
                     &msgshadowcolor,
                     shadowXOffset, shadowYOffset);
    XFlush(Dpy);
    XftDrawDestroy(draw);
}

void Panel::Error(const string& text) {
    ClosePanel();
    Message(text);
    sleep(ERROR_DURATION);
    OpenPanel();
    ClearPanel();
}


unsigned long Panel::GetColor(const char* colorname) {
    XColor color;
    XWindowAttributes attributes;

    if (mode == Mode_Lock) {
        XGetWindowAttributes(Dpy, Win, &attributes);
    } else {
        XGetWindowAttributes(Dpy, Root, &attributes);
    }
    color.pixel = 0;

    if(!XParseColor(Dpy, attributes.colormap, colorname, &color))
        logStream << APPNAME << ": can't parse color " << colorname << endl;
    else if(!XAllocColor(Dpy, attributes.colormap, &color))
        logStream << APPNAME << ": can't allocate color " << colorname << endl;

    return color.pixel;
}

void Panel::Cursor(int visible) {
    const char* text;
    int xx = 0, yy = 0, y2 = 0, cheight = 0;
    const char* txth = "Wj"; // used to get cursor height

    if (mode == Mode_Lock) {
        text = HiddenPasswdBuffer.c_str();
        xx = input_pass.x;
        yy = input_pass.y;
    } else switch(field) {
        case Get_Passwd:
            text = HiddenPasswdBuffer.c_str();
            xx = input_pass.x;
            yy = input_pass.y;
            break;

        case Get_Name:
            text = NameBuffer.c_str();
            xx = input_name.x;
            yy = input_name.y;
            break;
    }

    XGlyphInfo extents;
    XftTextExtents8(Dpy, font, (XftChar8*)txth, strlen(txth), &extents);
    cheight = extents.height;
    y2 = yy - extents.y + extents.height;
    XftTextExtents8(Dpy, font, (XftChar8*)text, strlen(text), &extents);
    xx += extents.width;

    if(visible == SHOW) {
        if (mode == Mode_Lock) {
            xx += viewport.x;
            yy += viewport.y;
            y2 += viewport.y;
        }
        XSetForeground(Dpy, TextGC,
                       GetColor(cfg->getOption("input_color").c_str()));
        XDrawLine(Dpy, Win, TextGC,
                  xx+1, yy-cheight,
                  xx+1, y2);
    } else {
        if (mode == Mode_Lock) {
           ApplyBackground(Rectangle(xx+1, yy-cheight,
                       1, y2-(yy-cheight)+1));
        } else {
            XClearArea(Dpy, Win, xx+1, yy-cheight,
                       1, y2-(yy-cheight)+1, false);
        }
    }
}

void Panel::EventHandler(const Panel::FieldType& curfield) {
    XEvent event;
    field=curfield;
    bool loop = true;

    if (mode == Mode_DM) {
        OnExpose();
    }

    struct pollfd x11_pfd = {0};
    x11_pfd.fd = ConnectionNumber(Dpy);
    x11_pfd.events = POLLIN;
    uint64_t last_time = CurrentEpochms();
    while(loop) {
        if (mode == Mode_Lock)
            UpdateTextWidget(&last_time);
        if(XPending(Dpy) || poll(&x11_pfd, 1, -1) > 0) {
            while(XPending(Dpy)) {
                XNextEvent(Dpy, &event);
                switch(event.type) {
                    case Expose:
                        OnExpose();
                        break;

                    case KeyPress:
                        loop=OnKeyPress(event);
                        break;
                }
            }
        }
    }

    return;
}

void Panel::OnExpose(void) {
    XftDraw *draw = XftDrawCreate(Dpy, Win,
                        DefaultVisual(Dpy, Scr), DefaultColormap(Dpy, Scr));

    if (mode == Mode_Lock) {
        ApplyBackground();
    } else {
        XClearWindow(Dpy, Win);
    }

    if (input_pass.x != input_name.x || input_pass.y != input_name.y){
        SlimDrawString8 (draw, &inputcolor, font, input_name.x, input_name.y,
                         NameBuffer,
                         &inputshadowcolor,
                         inputShadowOffset.x, inputShadowOffset.y);
        SlimDrawString8 (draw, &inputcolor, font, input_pass.x, input_pass.y,
                         HiddenPasswdBuffer,
                         &inputshadowcolor,
                         inputShadowOffset.x, inputShadowOffset.y);
    } else { //single input mode
        switch(field) {
            case Get_Passwd:
                SlimDrawString8 (draw, &inputcolor, font,
                                 input_pass.x, input_pass.y,
                                 HiddenPasswdBuffer,
                                 &inputshadowcolor,
                                 inputShadowOffset.x, inputShadowOffset.y);
                break;
            case Get_Name:
                SlimDrawString8 (draw, &inputcolor, font,
                                 input_name.x, input_name.y,
                                 NameBuffer,
                                 &inputshadowcolor,
                                 inputShadowOffset.x, inputShadowOffset.y);
                break;
        }
    }

    XftDrawDestroy (draw);
    Cursor(SHOW);
    ShowText();
}

bool Panel::OnKeyPress(XEvent& event) {
    char ascii;
    KeySym keysym;
    XComposeStatus compstatus;
    int xx = 0;
    int yy = 0;
    string text;
    string formerString = "";

    XLookupString(&event.xkey, &ascii, 1, &keysym, &compstatus);
    switch(keysym){
        case XK_F1:
            SwitchSession();
            return true;

        case XK_F11:
            // Take a screenshot
            system(cfg->getOption("screenshot_cmd").c_str());
            return true;

        case XK_Return:
        case XK_KP_Enter:
            if (field==Get_Name){
                // Don't allow an empty username
                if (NameBuffer.empty()) return true;

                if (NameBuffer==CONSOLE_STR){
                    action = Console;
                } else if (NameBuffer==HALT_STR){
                    action = Halt;
                } else if (NameBuffer==REBOOT_STR){
                    action = Reboot;
                } else if (NameBuffer==SUSPEND_STR){
                    action = Suspend;
                } else if (NameBuffer==EXIT_STR){
                    action = Exit;
                } else {
                    if (mode == Mode_DM) {
                        action = Login;
                    } else {
                        action = Lock;
                    }
                }
            };
            return false;
        default:
            break;
    };

    Cursor(HIDE);
    switch(keysym){
        case XK_Delete:
        case XK_BackSpace:
            switch(field) {
                case GET_NAME:
                    if (! NameBuffer.empty()){
                        formerString=NameBuffer;
                        NameBuffer.erase(--NameBuffer.end());
                    };
                    break;
                case GET_PASSWD:
                    if (! PasswdBuffer.empty()){
                        formerString=HiddenPasswdBuffer;
                        PasswdBuffer.erase(--PasswdBuffer.end());
                        HiddenPasswdBuffer.erase(--HiddenPasswdBuffer.end());
                    };
                    break;
            };
            break;

        case XK_w:
        case XK_u:
            if (reinterpret_cast<XKeyEvent&>(event).state & ControlMask) {
                switch(field) {
                    case Get_Passwd:
                        formerString = HiddenPasswdBuffer;
                        HiddenPasswdBuffer.clear();
                        PasswdBuffer.clear();
                        break;

                    case Get_Name:
                        formerString = NameBuffer;
                        NameBuffer.clear();
                        break;
                }
                break;
            }
            // Deliberate fall-through (??)
        case XK_h:
            if (reinterpret_cast<XKeyEvent&>(event).state & ControlMask) {
                // note - this is a copy of code from XK_BackSpace
                switch(field) {
                    case GET_NAME:
                        if (! NameBuffer.empty()){
                            formerString=NameBuffer;
                            NameBuffer.erase(--NameBuffer.end());
                        };
                        break;
                    case GET_PASSWD:
                        if (! PasswdBuffer.empty()){
                            formerString=HiddenPasswdBuffer;
                            PasswdBuffer.erase(--PasswdBuffer.end());
                            HiddenPasswdBuffer.erase(--HiddenPasswdBuffer.end());
                        };
                        break;
                };
                break;
            }
            // Deliberate fall-through

        default:
            if (isprint(ascii) && (keysym < XK_Shift_L || keysym > XK_Hyper_R)){
                switch(field) {
                    case GET_NAME:
                        formerString=NameBuffer;
                        if (NameBuffer.length() < INPUT_MAXLENGTH_NAME-1){
                            NameBuffer.append(&ascii,1);
                        };
                        break;
                    case GET_PASSWD:
                        formerString=HiddenPasswdBuffer;
                        if (PasswdBuffer.length() < INPUT_MAXLENGTH_PASSWD-1){
                            PasswdBuffer.append(&ascii,1);
                            HiddenPasswdBuffer.append("*");
                        };
                    break;
                };
            };
            break;
    };

    XGlyphInfo extents;
    XftDraw *draw = XftDrawCreate(Dpy, Win,
                                  DefaultVisual(Dpy, Scr), DefaultColormap(Dpy, Scr));

   switch(field) {
        case Get_Name:
            text = NameBuffer;
            xx = input_name.x;
            yy = input_name.y;
            break;

        case Get_Passwd:
            text = HiddenPasswdBuffer;
            xx = input_pass.x;
            yy = input_pass.y;
            break;
    }

    if (!formerString.empty()){
        const char* txth = "Wj"; // get proper maximum height ?
        XftTextExtents8(Dpy, font, reinterpret_cast<const XftChar8*>(txth), strlen(txth), &extents);
        int maxHeight = extents.height;

        XftTextExtents8(Dpy, font, reinterpret_cast<const XftChar8*>(formerString.c_str()),
                        formerString.length(), &extents);
        int maxLength = extents.width;

        if (mode == Mode_Lock) {
            ApplyBackground(Rectangle(input_pass.x - 3,
                    input_pass.y - maxHeight - 3,
                    maxLength + 6, maxHeight + 6));
        } else {
            XClearArea(Dpy, Win, xx-3, yy-maxHeight-3,
                       maxLength+6, maxHeight+6, false);
        }
    }

    if (!text.empty()) {
        SlimDrawString8 (draw, &inputcolor, font, xx, yy,
                         text,
                         &inputshadowcolor,
                         inputShadowOffset.x, inputShadowOffset.y);
    }

    XftDrawDestroy (draw);
    Cursor(SHOW);
    return true;
}

// Draw welcome and "enter username" message
void Panel::ShowText(){
    string cfgX, cfgY;
    XGlyphInfo extents;

    bool singleInputMode =
    input_name.x == input_pass.x &&
    input_name.y == input_pass.y;

    XftDraw *draw = XftDrawCreate(Dpy, Win,
                                  DefaultVisual(Dpy, Scr), DefaultColormap(Dpy, Scr));
    /* welcome message */
    XftTextExtents8(Dpy, welcomefont, (XftChar8*)welcome_message.c_str(),
                    strlen(welcome_message.c_str()), &extents);
    cfgX = cfg->getOption("welcome_x");
    cfgY = cfg->getOption("welcome_y");
    int shadowXOffset = cfg->getIntOption("welcome_shadow_xoffset");
    int shadowYOffset = cfg->getIntOption("welcome_shadow_yoffset");

    welcome.x = Cfg::absolutepos(cfgX, image->Width(), extents.width);
    welcome.y = Cfg::absolutepos(cfgY, image->Height(), extents.height);
    if (welcome.x >= 0 && welcome.y >= 0) {
        SlimDrawString8 (draw, &welcomecolor, welcomefont,
                         welcome.x, welcome.y,
                         welcome_message,
                         &welcomeshadowcolor, shadowXOffset, shadowYOffset);
    }

    /* Enter username-password message */
    string msg;
    if ((!singleInputMode|| field == Get_Passwd) && mode == Mode_DM) {
        msg = cfg->getOption("password_msg");
        XftTextExtents8(Dpy, enterfont, (XftChar8*)msg.c_str(),
                        strlen(msg.c_str()), &extents);
        cfgX = cfg->getOption("password_x");
        cfgY = cfg->getOption("password_y");
        int shadowXOffset = cfg->getIntOption("username_shadow_xoffset");
        int shadowYOffset = cfg->getIntOption("username_shadow_yoffset");
        password.x = Cfg::absolutepos(cfgX, image->Width(), extents.width);
        password.y = Cfg::absolutepos(cfgY, image->Height(), extents.height);
        if (password.x >= 0 && password.y >= 0){
            SlimDrawString8 (draw, &entercolor, enterfont, password.x, password.y,
                             msg, &entershadowcolor, shadowXOffset, shadowYOffset);
        }
    }
    if (!singleInputMode|| field == Get_Name ) {
        msg = cfg->getOption("username_msg");
        XftTextExtents8(Dpy, enterfont, (XftChar8*)msg.c_str(),
                        strlen(msg.c_str()), &extents);
        cfgX = cfg->getOption("username_x");
        cfgY = cfg->getOption("username_y");
        int shadowXOffset = cfg->getIntOption("username_shadow_xoffset");
        int shadowYOffset = cfg->getIntOption("username_shadow_yoffset");
        username.x = Cfg::absolutepos(cfgX, image->Width(), extents.width);
        username.y = Cfg::absolutepos(cfgY, image->Height(), extents.height);
        if (username.x >= 0 && username.y >= 0){
            SlimDrawString8 (draw, &entercolor, enterfont, username.x, username.y,
                             msg, &entershadowcolor, shadowXOffset, shadowYOffset);
        }
    }
    XftDrawDestroy(draw);

    if (mode == Mode_Lock) {
        // If only the password box is visible, draw the user name somewhere too
        string user_msg = "User: " + GetName();
        int show_username = cfg->getIntOption("show_username");
        if (singleInputMode && show_username) {
            Message(user_msg);
        }
    }
}

string Panel::getSession() {
    return session_exec;
}

// choose next available session type
void Panel::SwitchSession() {
    pair<string,string> ses = cfg->nextSession();
    session_name = ses.first;
    session_exec = ses.second;
    if (session_name.size() > 0) {
        ShowSession();
    }
}

// Display session type on the screen
void Panel::ShowSession() {
	string msg_x, msg_y;
    XClearWindow(Dpy, Root);
    string currsession = cfg->getOption("session_msg") + " " + session_name;
    XGlyphInfo extents;

	sessionfont = XftFontOpenName(Dpy, Scr, cfg->getOption("session_font").c_str());

	XftDraw *draw = XftDrawCreate(Dpy, Root,
                                  DefaultVisual(Dpy, Scr), DefaultColormap(Dpy, Scr));
    XftTextExtents8(Dpy, sessionfont, reinterpret_cast<const XftChar8*>(currsession.c_str()),
                    currsession.length(), &extents);
    msg_x = cfg->getOption("session_x");
    msg_y = cfg->getOption("session_y");
    int x = Cfg::absolutepos(msg_x, XWidthOfScreen(ScreenOfDisplay(Dpy, Scr)), extents.width);
    int y = Cfg::absolutepos(msg_y, XHeightOfScreen(ScreenOfDisplay(Dpy, Scr)), extents.height);
    int shadowXOffset = cfg->getIntOption("session_shadow_xoffset");
    int shadowYOffset = cfg->getIntOption("session_shadow_yoffset");

    SlimDrawString8(draw, &sessioncolor, sessionfont, x, y,
                    currsession,
                    &sessionshadowcolor,
                    shadowXOffset, shadowYOffset);
    XFlush(Dpy);
    XftDrawDestroy(draw);
}


void Panel::SlimDrawString8(XftDraw *d, XftColor *color, XftFont *font,
                            int x, int y, const string& str,
                            XftColor* shadowColor,
                            int xOffset, int yOffset)
{
    int calc_x = 0;
    int calc_y = 0;
    if (mode == Mode_Lock) {
        calc_x = viewport.x;
        calc_y = viewport.y;
    }

    if (xOffset && yOffset) {
        XftDrawStringUtf8(d, shadowColor, font,
                          x + xOffset + calc_x,
                          y + yOffset + calc_y,
                          reinterpret_cast<const FcChar8*>(str.c_str()), str.length());
    }
    XftDrawStringUtf8(d, color, font, x + calc_x, y + calc_y,
                      reinterpret_cast<const FcChar8*>(str.c_str()), str.length());
}

Panel::ActionType Panel::getAction(void) const{
    return action;
};

void Panel::Reset(void){
    ResetName();
    ResetPasswd();
};

void Panel::ResetName(void){
    NameBuffer.clear();
};

void Panel::ResetPasswd(void){
    PasswdBuffer.clear();
    HiddenPasswdBuffer.clear();
};

void Panel::SetName(const string& name){
    NameBuffer=name;
    if (mode == Mode_DM) {
        action = Login;
    } else {
        action = Lock;
    }
};

const string& Panel::GetName(void) const{
    return NameBuffer;
};

const string& Panel::GetPasswd(void) const{
    return PasswdBuffer;
};

Rectangle Panel::GetPrimaryViewport() {
    Rectangle fallback;
    Rectangle result;

    RROutput primary;
    XRROutputInfo *primary_info;
    XRRScreenResources *resources;
    XRRCrtcInfo *crtc_info;

    int crtc;

    fallback.x = 0;
    fallback.y = 0;
    fallback.width = DisplayWidth(Dpy, Scr);
    fallback.height = DisplayHeight(Dpy, Scr);

    primary = XRRGetOutputPrimary(Dpy, Win);
    if (!primary) {
        return fallback;
    }
    resources = XRRGetScreenResources(Dpy, Win);
    if (!resources)
        return fallback;

    primary_info = XRRGetOutputInfo(Dpy, resources, primary);
        if (!primary_info) {
            XRRFreeScreenResources(resources);
            return fallback;
        }

    // Fixes bug with multiple monitors.  Just pick first monitor if
    // XRRGetOutputInfo gives returns bad into for crtc.
    if (primary_info->crtc < 1) {
        if (primary_info->ncrtc > 0) {
            crtc = primary_info->crtcs[0];
        } else {
            cerr << "Cannot get crtc from xrandr.\n";
            exit(EXIT_FAILURE);
        }
    } else {
        crtc = primary_info->crtc;
    }

    crtc_info = XRRGetCrtcInfo(Dpy, resources, crtc);

    if (!crtc_info) {
        XRRFreeOutputInfo(primary_info);
        XRRFreeScreenResources(resources);
        return fallback;
    }

    result.x = crtc_info->x;
    result.y = crtc_info->y;
    result.width = crtc_info->width;
    result.height = crtc_info->height;

    XRRFreeCrtcInfo(crtc_info);
    XRRFreeOutputInfo(primary_info);
    XRRFreeScreenResources(resources);

    return result;
};

void Panel::ApplyBackground(Rectangle rect) {
    int ret = 0;

    if (rect.is_empty()) {
        rect.x = 0;
        rect.y = 0;
        rect.width = viewport.width;
        rect.height = viewport.height;
    }

    ret = XCopyArea(Dpy, PanelPixmap, Win, WinGC,
            rect.x, rect.y, rect.width, rect.height,
            viewport.x + rect.x, viewport.y + rect.y);

    if (!ret) {
        cerr << APPNAME << ": failed to put pixmap on the screen\n.";
    }
};

std::string Panel::Execute(const char* cmd) {
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
    if (!pipe) {
        throw std::runtime_error("popen() failed!");
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

void Panel::CalcPos(std::string cfgX, std::string cfgY, XGlyphInfo extents, Rectangle *rect)
{
    if (mode == Mode_Lock) {
        rect->x = Cfg::absolutepos(cfgX, viewport.width, extents.width);
        rect->y = Cfg::absolutepos(cfgY, viewport.height, extents.height);
    } else {
        rect->x = Cfg::absolutepos(cfgX, XWidthOfScreen(ScreenOfDisplay(Dpy, Scr)), extents.width);
        rect->y = Cfg::absolutepos(cfgY, XHeightOfScreen(ScreenOfDisplay(Dpy, Scr)), extents.height);
    }
}

uint64_t Panel::CurrentEpochms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void Panel::UpdateTextWidget(uint64_t *last_time)
{
    uint64_t current_time = CurrentEpochms();
    if ((current_time - (*last_time)) >= (text_widget_interval * 1000.0))
    {
        string cfgX, cfgY;
        XGlyphInfo extents;
        Rectangle rect;

        Window target = (mode == Mode_Lock) ? Win : Root;
        XftDraw *draw = XftDrawCreate(Dpy, target,
                                      DefaultVisual(Dpy, Scr), DefaultColormap(Dpy, Scr));
        cfgX = cfg->getOption("text_widget_x");
        cfgY = cfg->getOption("text_widget_y");
        XftTextExtentsUtf8(Dpy, text_widget_font,
                           reinterpret_cast<const XftChar8*>(text_widget_former_string.c_str()),
                           text_widget_former_string.length(), &extents);
        CalcPos(cfgX, cfgY, extents, &rect);
        rect.y = rect.y - extents.height;
        rect.width = extents.width + text_widget_shadow_offset.x;
        rect.height = extents.height + text_widget_shadow_offset.y;

        int ret = XCopyArea(Dpy, PanelPixmap, target, WinGC,
                            rect.x, rect.y, rect.width, rect.height,
                            rect.x, rect.y);

        std::string cmd_result = Execute(text_widget_command);

        XftTextExtentsUtf8(Dpy, text_widget_font, reinterpret_cast<const XftChar8*>(cmd_result.c_str()),
                           cmd_result.length(), &extents);
        CalcPos(cfgX, cfgY, extents, &rect);
        SlimDrawString8(draw, &text_widget_color, text_widget_font, rect.x, rect.y,
                        cmd_result, &text_widget_shadow_color,
                        text_widget_shadow_offset.x, text_widget_shadow_offset.y);
        XFlush(Dpy);
        XftDrawDestroy(draw);
        *last_time = current_time;
        text_widget_former_string = cmd_result;
    }
}
