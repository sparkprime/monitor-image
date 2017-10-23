#define _POSIX_C_SOURCE 199309L

#include <cstdlib>

#include <iostream>
#include <string>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <sys/inotify.h>
#include <limits.h>
#include <unistd.h>

extern "C" {
    #include <FreeImage.h>
}

int redraw(Display * const display, const Pixmap pixmap, const Window window, const GC gc,
           int img_width, int img_height)
{
    XWindowAttributes attrs;
    if (!XGetWindowAttributes(display, window, &attrs)) {
        return EXIT_FAILURE;
    }

    Colormap colormap = DefaultColormap(display, DefaultScreen(display));

    XColor black;
    if (!XAllocNamedColor(display, colormap, "black", &black, &black)) {
        std::cerr << "XAllocNamedColor - failed to allocated 'black' color" << std::endl;
        return EXIT_FAILURE;
    }


    XSetForeground(display, gc, black.pixel);
    XFillRectangle(display, window, gc, 0, 0, attrs.width, attrs.height);

    XCopyArea(display, pixmap, window, gc, 0, 0, img_width, img_height, 0, 0);

    return EXIT_SUCCESS;
}

int load_image (Display * const display, const Window window, const GC gc, Pixmap &pixmap,
                int &width, int &height, const std::string &filename)
{
    FREE_IMAGE_FORMAT fif = FreeImage_GetFileType(filename.c_str(), 0);
    if (fif == FIF_UNKNOWN) {
        fif = FreeImage_GetFIFFromFilename(filename.c_str());
    }
    if (fif == FIF_UNKNOWN) {
        std::cerr << "Unknown format: " << filename << std::endl;
        return EXIT_FAILURE;
    }
    if (!FreeImage_FIFSupportsReading(fif)) {
        std::cerr << "Couldn't read format: " << filename << std::endl;
    }

    FIBITMAP * const input = FreeImage_Load(fif, filename.c_str());
    if (input == NULL) {
        return EXIT_FAILURE;
    }

    const FREE_IMAGE_TYPE input_type = FreeImage_GetImageType(input);
    width = FreeImage_GetWidth(input);
    height = FreeImage_GetHeight(input);


    if (input_type != FIT_BITMAP) {
        std::cerr << "Unsupported format: " << filename << std::endl;
        return EXIT_FAILURE;
    }

    const int depth = FreeImage_GetBPP(input);
    const int depth_bytes = depth / 8;

    const int bitmap_pad = 32; // 32 for 24 and 32 bpp, 16, for 15&16
    const int pad_bytes = bitmap_pad / 8;

    // Use a 32bit image no matter what we loaded.
    auto *image32 = (char*)malloc(width * height * pad_bytes);

    switch (depth) {
        case 24: case 32: {
            int channel_offset[4] = { FI_RGBA_BLUE, FI_RGBA_GREEN, FI_RGBA_RED, FI_RGBA_ALPHA };
            for (int y = 0 ; y < height ; y++) {
                BYTE *raw = FreeImage_GetScanLine(input, height - y - 1);
                for (int x = 0 ; x < width ; x++) {
                    unsigned char alpha = depth_bytes == 3 ? 255 : raw[channel_offset[3]];
                    for (int c = 0 ; c < 3 ; ++c) {
                        image32[(y * width + x) * pad_bytes + c] = (int(raw[channel_offset[c]]) * alpha) / 255;
                    }
                    image32[(y * width + x) * pad_bytes + 3] = 255;
                    raw += depth_bytes;
                }
            }
        } break;

        case 8: {
            for (int y = 0 ; y < height ; y++) {
                BYTE *raw = FreeImage_GetScanLine(input, height - y - 1);
                for (int x = 0 ; x < width ; x++) {
                    image32[(y * width + x) * pad_bytes + 0] = raw[x];
                    image32[(y * width + x) * pad_bytes + 1] = raw[x];
                    image32[(y * width + x) * pad_bytes + 2] = raw[x];
                    image32[(y * width + x) * pad_bytes + 3] = 255;
                }
            }
        } break;

        
        default:
        std::cerr << "Couldn't read " << filename
                  << ": Only 32bit, 24bit, and 8bit images supported"
                  << " got " << depth << "." << std::endl;
        return EXIT_FAILURE;
    }

    FreeImage_Unload(input);

    int bytes_per_line = 0; // number of bytes in the client image between the start of one scanline and the start of the next
    XImage *image = XCreateImage(
        display, CopyFromParent, 24, ZPixmap, 0, image32, width, height, bitmap_pad,
        bytes_per_line);

    if (pixmap)
        XFreePixmap(display, pixmap);
    pixmap = XCreatePixmap(display, window, width, height, 24);
    XPutImage(display, pixmap, gc, image, 0, 0, 0, 0, width, height);
    XDestroyImage(image);

    return EXIT_SUCCESS;
}

int main(int argc, const char **argv)
{

    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <filename>" << std::endl;
        return EXIT_FAILURE;
    }

    const std::string filename = argv[1];

    Display * const display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        std::cerr << "Cannot open display" << std::endl;
        return EXIT_FAILURE;
    }

    const int screen = DefaultScreen(display);

    const Window window = XCreateSimpleWindow(
        display, RootWindow(display, screen), 10, 10, 100, 100, 1,
        BlackPixel(display, screen), BlackPixel(display, screen));

    // Process Window Close Event through event handler so XNextEvent does Not fail
    Atom del_window = XInternAtom(display, "WM_DELETE_WINDOW", 0);
    XSetWMProtocols(display, window, &del_window, 1);

    // Events we handle:
    XSelectInput(display, window, ExposureMask | KeyPressMask);

    XMapWindow(display, window);

    XGCValues gcvalues;
    const GC gc = XCreateGC(display, window, 0, &gcvalues);

    int width, height;
    Pixmap pixmap = 0;

    int status = load_image(display, window, gc, pixmap, width, height, filename);
    if (status != EXIT_SUCCESS) {
        return status;
    }



    int inotifyFd = inotify_init1(IN_NONBLOCK);
    if (inotifyFd == -1) {
        std::cerr << "Could not initialize inotify." << std::endl;
        return EXIT_FAILURE;
    }
 
    /* For each command-line argument, add a watch for all events */
    int wd = inotify_add_watch(inotifyFd, filename.c_str(), IN_CLOSE_WRITE);
    if (wd == -1) {
        std::cerr << "Could not add watch." << std::endl;
        return EXIT_FAILURE;
    }

    while (true) {
        constexpr size_t buf_len = (10 * (sizeof(struct inotify_event) + NAME_MAX + 1));
        char buf[buf_len] __attribute__ ((aligned(8)));
        int numRead = read(inotifyFd, buf, buf_len);
        if (numRead == 0) {
            std::cerr << "Read nothing from inotify." << std::endl;
            return EXIT_FAILURE;
        }
        if (numRead == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // No events.
            } else {
                std::cerr << "Error reading inotify." << std::endl;
                return EXIT_FAILURE;
            }
        } else {
            // Reload image.
            int status = load_image(display, window, gc, pixmap, width, height, filename);
            if (status != EXIT_SUCCESS) {
                return status;
            }

            redraw(display, pixmap, window, gc, width, height);
        }

        // Catch up on display.
        while (XPending(display)) {

            XEvent e;
            XNextEvent(display, &e);

            if (e.type == Expose) {
                redraw(display, pixmap, window, gc, width, height);
            }

            if (e.type == ClientMessage) {
                goto exit; 
            }
        }

        struct timespec sleep_time;
        sleep_time.tv_sec = 0;
        sleep_time.tv_nsec = 50 * 1000 * 1000;
        nanosleep(&sleep_time, nullptr);
    }

    exit:

    if (pixmap)
        XFreePixmap(display, pixmap);
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return EXIT_SUCCESS;
}

