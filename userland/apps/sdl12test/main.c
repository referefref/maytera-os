/* sdl12test - proof-of-life for the MayteraOS SDL 1.2 backend (task #745,
 * docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md Tier 2 #7). Compiles
 * against the pinned upstream SDL 1.2 headers UNMODIFIED and exercises:
 *   - SDL_Init / SDL_SetVideoMode (software surface)
 *   - SDL_CreateRGBSurface / SDL_FillRect / SDL_BlitSurface / SDL_Flip
 *   - SDL_PollEvent / SDL_KEYDOWN / SDLK_* (keyboard responds visibly:
 *     arrow keys change the background colour, Escape quits)
 *   - SDL_GetTicks-driven continuous animation (a bouncing sprite and a
 *     progress bar), so two screendumps a few seconds apart show motion
 *   - a second SDL_SetVideoMode(..., SDL_OPENGL) call and a real TinyGL
 *     triangle drawn through SDL_GL_SwapBuffers, proving the GL path
 *
 * This is a test harness, not a class-E port: a real port would replace an
 * existing hand-written sdlshim.cpp (see the backend's own README for which
 * two, and why neither could be swapped in this same pass).
 *
 * No em-dashes (repo writing-style rule).
 */
#include "SDL.h"
#include "GL/gl.h"

#define SCR_W 640
#define SCR_H 480

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 1;
    SDL_WM_SetCaption("SDL 1.2 Backend Test", "sdl12test");

    SDL_Surface *screen = SDL_SetVideoMode(SCR_W, SCR_H, 32, SDL_SWSURFACE);
    if (!screen) { SDL_Quit(); return 1; }

    SDL_Surface *sprite = SDL_CreateRGBSurface(SDL_SWSURFACE, 48, 48, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0);
    if (sprite) SDL_FillRect(sprite, 0, SDL_MapRGB(sprite->format, 255, 220, 0));

    int sx = 40, sy = 40, dx = 3, dy = 2;
    Uint32 bg = SDL_MapRGB(screen->format, 20, 20, 30);
    int quit = 0;
    int frame = 0;

    while (!quit && frame < 3000) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) quit = 1;
            else if (e.type == SDL_KEYDOWN) {
                switch (e.key.keysym.sym) {
                case SDLK_ESCAPE: quit = 1; break;
                case SDLK_UP:    bg = SDL_MapRGB(screen->format, 40, 60, 180); break;
                case SDLK_DOWN:  bg = SDL_MapRGB(screen->format, 40, 150, 60); break;
                case SDLK_LEFT:  bg = SDL_MapRGB(screen->format, 200, 110, 20); break;
                case SDLK_RIGHT: bg = SDL_MapRGB(screen->format, 150, 40, 180); break;
                case SDLK_SPACE: bg = SDL_MapRGB(screen->format, 250, 250, 250); break;
                default: break;
                }
            }
        }
        SDL_FillRect(screen, 0, bg);

        sx += dx; sy += dy;
        if (sx < 0 || sx + 48 > SCR_W) { dx = -dx; sx += dx; }
        if (sy < 0 || sy + 48 > SCR_H) { dy = -dy; sy += dy; }
        if (sprite) {
            SDL_Rect dst; dst.x = (Sint16)sx; dst.y = (Sint16)sy; dst.w = 0; dst.h = 0;
            SDL_BlitSurface(sprite, 0, screen, &dst);
        }

        /* A progress bar driven by SDL_GetTicks(), so any two screendumps
         * taken seconds apart visibly differ even with no key pressed. */
        Uint32 t = SDL_GetTicks() % 4000;
        SDL_Rect bar; bar.x = 0; bar.y = 0; bar.w = (Uint16)((t * (Uint32)SCR_W) / 4000); bar.h = 8;
        SDL_FillRect(screen, &bar, SDL_MapRGB(screen->format, 255, 255, 255));

        SDL_Flip(screen);
        SDL_Delay(16);
        frame++;
    }
    if (sprite) SDL_FreeSurface(sprite);

    if (!quit) {
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
        SDL_Surface *glscreen = SDL_SetVideoMode(SCR_W, SCR_H, 32, SDL_OPENGL);
        if (glscreen) {
            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            double aspect = (double)SCR_W / (double)SCR_H;
            glFrustum(-aspect, aspect, -1.0, 1.0, 1.5, 60.0);
            glMatrixMode(GL_MODELVIEW);

            float ang = 0.0f, spin = 2.0f;
            int gframe = 0;
            while (!quit && gframe < 3000) {
                SDL_Event e;
                while (SDL_PollEvent(&e)) {
                    if (e.type == SDL_QUIT) quit = 1;
                    else if (e.type == SDL_KEYDOWN) {
                        if (e.key.keysym.sym == SDLK_ESCAPE) quit = 1;
                        else if (e.key.keysym.sym == SDLK_LEFT) spin = -2.0f;
                        else if (e.key.keysym.sym == SDLK_RIGHT) spin = 2.0f;
                        else if (e.key.keysym.sym == SDLK_UP) spin *= 1.5f;
                        else if (e.key.keysym.sym == SDLK_DOWN) spin *= 0.6f;
                    }
                }
                glClearColor(0.05f, 0.05f, 0.12f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                glLoadIdentity();
                glTranslatef(0.0f, 0.0f, -4.0f);
                glRotatef(ang, 0.0f, 1.0f, 0.0f);
                glBegin(GL_TRIANGLES);
                glColor3f(1.0f, 0.0f, 0.0f); glVertex3f(0.0f, 1.0f, 0.0f);
                glColor3f(0.0f, 1.0f, 0.0f); glVertex3f(-1.0f, -1.0f, 0.0f);
                glColor3f(0.0f, 0.0f, 1.0f); glVertex3f(1.0f, -1.0f, 0.0f);
                glEnd();
                SDL_GL_SwapBuffers();
                SDL_Delay(16);
                ang += spin;
                gframe++;
            }
        }
    }

    SDL_Quit();
    return 0;
}
