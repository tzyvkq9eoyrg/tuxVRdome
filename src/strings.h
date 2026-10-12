#pragma once

enum class Language { German, English };

// All text shown in VR, per language.
struct Strings {
  const char* noVideo;
  const char* loading;
  const char* format;
  const char* projections[6];
  const char* stereo[3];
  const char* curved;
  const char* recenter;
  const char* settings;
  const char* files;
  const char* back;
  const char* noVideos;
  const char* noSubtitleFiles;
  const char* chooseSubtitleFile;
  const char* noAudio;
  const char* subtitles;
  const char* off;
  const char* on;
  const char* track;
  const char* language;
  const char* subtitles3d;
  const char* subtitles3dHint;
  const char* subtitleFromFile;
  const char* youtube;
  const char* search;
  const char* vrOnly;
  const char* searching;
  const char* loadingMore;
  const char* noResults;
  const char* searchFailed;
  const char* space;
  const char* paste;
  const char* typeToSearch;
  const char* live;
  const char* qualityMax;
  const char* defaultQuality;
  const char* defaultQualityHint;
  const char* lyingMode;
  const char* lyingModeHint;
  const char* deleteTitle;
  const char* deleteToTrashHint;
  const char* deletePermanentWarning;
  const char* trashFailedWarning;
  const char* deleteFailed;
  const char* cancel;
  const char* moveToTrash;
  const char* deletePermanently;
  const char* ok;
  const char* quitTitle;
  const char* quit;
  const char* picture;
  const char* pictureHint;
  const char* brightness;
  const char* contrast;
  const char* saturation;
  const char* zoom;
  const char* reset;
  const char* loopModes[3];  // tooltips: off, video, folder
  const char* loopShort[3];  // the same on the button
  const char* loopPlaylist;  // the folder mode for a playing YouTube playlist
  const char* loopPlaylistShort;
  const char* sleepOff;
  const char* design;
  const char* themes[2];            // glass, material
  const char* projectionShort[6];   // format tiles
  const char* stereoShort[3];
  // Tooltips of icon buttons.
  const char* tipDelete;
  const char* tipPrevious;
  const char* tipNext;
  const char* tipBack10;
  const char* tipForward10;
  const char* tipPlay;
  const char* tipPause;
  const char* tipMute;
  const char* tipSpeed;
  const char* tipAbLoop;
  const char* tipSleep;
  const char* tipSlideshow;
  const char* deletePhotoTitle;
  const char* tipDeletePhoto;
  const char* sortModes[5];  // name A-Z, name Z-A, newest, oldest, largest
  const char* tipTop;
  const char* tipBottom;
  const char* tipGallery;
  const char* tipList;
  const char* background;
  const char* recenterIn;  // printf format with one %d (seconds)
  const char* subtitleSize;
  const char* subtitleRaise;
  const char* subtitleDistance;
  const char* menuFollows;
  const char* tipCollapse;
  const char* tipExpand;
  const char* chapter;
  const char* favorites;
  const char* noFavorites;
  const char* tipFavorite;
  const char* tipUnfavorite;
};

const Strings& strings(Language language);
