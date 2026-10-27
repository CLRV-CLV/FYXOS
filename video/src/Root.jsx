import React from 'react';
import {Audio, Composition, Sequence, staticFile} from 'remotion';
import {AppImage, Architecture, BootProof, ColdOpen, Desktop, EndCard, Evidence, Honest, Release, Restore, SDK, Studio} from './scenes';

const chapters = [
  [ColdOpen, 288],
  [Architecture, 336],
  [Desktop, 432],
  [BootProof, 336],
  [SDK, 408],
  [Studio, 432],
  [AppImage, 408],
  [Restore, 432],
  [Evidence, 432],
  [Honest, 336],
  [Release, 288],
  [EndCard, 192],
];

export const Launch = () => {
  let from = 0;
  return <div style={{position: 'absolute', inset: 0, background: '#050809'}}>
    <Audio src={staticFile('audio/fyxos-score.m4a')} />
    {chapters.map(([Component, durationInFrames], index) => {
      const start = from;
      from += durationInFrames;
      return <Sequence key={index} from={start} durationInFrames={durationInFrames} name={`${String(index + 1).padStart(2, '0')} · ${Component.name}`}><Component /></Sequence>;
    })}
  </div>;
};

export const Root = () => <Composition id="FyxLaunch" component={Launch} durationInFrames={4320} fps={24} width={1920} height={1080} />;
